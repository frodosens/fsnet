#include "fsnet/entity_timers.hpp"
#include "fsnet/entity_record.hpp"
#include "entity_timer_index.hpp"
#include <algorithm>
#include <set>
#include <unordered_map>
#include <utility>

namespace fsnet {
struct EntityTimers::Impl:std::enable_shared_from_this<Impl> {
    using Clock=std::chrono::steady_clock;
    struct Watch {timer_index::Entry entry;std::int64_t next=0;std::uint64_t revision=0;bool busy=false,removed=false;};
    struct Scan {enum class Action{none,wake,fire} action=Action::none;std::int64_t next=0;std::optional<EntityTimer> timer;};
    std::shared_ptr<EntityScriptHost> host;AsyncDatabase storage;EntityTimerLimits limits;EntityTimerStats counters;
    std::unordered_map<std::string,std::shared_ptr<Watch>> watches;
    std::map<std::pair<std::int64_t,std::string>,std::shared_ptr<Watch>> schedule;
    std::vector<std::set<std::string>> members{timer_index::shards};
    Clock::time_point refresh{};unsigned cursor=0,remaining=0;bool stopped=false,polling=false;
    std::chrono::milliseconds idle{};std::uint64_t observed_revision=0,rounds=0;bool verify_hints=true;
    std::string owner,node;
    Impl(std::shared_ptr<EntityScriptHost> h,std::shared_ptr<Storage> db,EntityTimerLimits settings)
        :host(std::move(h)),storage(std::move(db)),limits(settings) {
        if(!host || limits.pending<2 || limits.pending>1024 || !limits.tracked || limits.tracked>timer_index::shards*timer_index::per_shard || limits.refresh.count()<=0 || limits.retry.count()<=0 || limits.refresh>std::chrono::hours(24) || limits.retry>std::chrono::hours(24))throw std::invalid_argument("invalid entity timer service limits");
        owner=host->owner_id();node=owner.substr(0,owner.find('/'));
        idle=limits.refresh;observed_revision=host->timer_revision();
    }
    void due(const std::shared_ptr<Watch>& watch,std::int64_t when) {
        auto key=timer_index::identity(watch->entry.type,watch->entry.id);
        schedule.erase({watch->next,key});watch->next=when;
        if(!watch->busy && !watch->removed)schedule.emplace(std::make_pair(when,std::move(key)),watch);
    }
    std::int64_t later(std::chrono::milliseconds duration)const {
        auto now=host->now_ms();return now>INT64_MAX-duration.count()?INT64_MAX:now+duration.count();
    }
    bool load(unsigned index) {
        ++counters.pending;auto bucket=std::make_shared<timer_index::Bucket>();
        auto complete=[weak=weak_from_this(),bucket,index](std::exception_ptr error){
            auto self=weak.lock();if(!self || self->stopped)return;--self->counters.pending;
            if(error){++self->counters.failures;return;}
            std::set<std::string> seen;
            for(const auto& [key,entry]:bucket->entries) {
                if(!self->host->schema(entry.type))continue;
                auto it=self->watches.find(key);
                if(it==self->watches.end()) {
                    if(self->watches.size()>=self->limits.tracked){
                        ++self->counters.capacity_failures;
                        // Prefer due work to a cached future/retrying task. Busy
                        // jobs retain their capacity until completion.
                        auto victim=self->schedule.empty()?self->schedule.end():std::prev(self->schedule.end());
                        if(victim==self->schedule.end() || victim->first.first<=entry.due_ms)continue;
                        victim->second->removed=true;self->watches.erase(victim->first.second);self->schedule.erase(victim);
                    }
                    auto watch=std::make_shared<Watch>();watch->entry=entry;it=self->watches.emplace(key,watch).first;
                    self->due(watch,entry.pending?self->host->now_ms():entry.due_ms);
                }else if(it->second->entry!=entry) {
                    auto watch=it->second;self->schedule.erase({watch->next,key});watch->entry=entry;++watch->revision;
                    self->due(watch,entry.pending?self->host->now_ms():entry.due_ms);
                }
                seen.insert(key);
            }
            if(!seen.empty()){self->idle=self->limits.refresh;self->refresh=Clock::now()+self->limits.refresh;}
            for(const auto& key:self->members[index])if(!seen.contains(key)) {
                auto it=self->watches.find(key);if(it==self->watches.end())continue;
                it->second->removed=true;self->schedule.erase({it->second->next,key});self->watches.erase(it);
            }
            self->members[index]=std::move(seen);
        };
        if(!storage.submit(3*1024*1024,[bucket,index,this_node=node,verify=verify_hints](Storage& db){
            *bucket=timer_index::read(db,index);
            for(auto it=bucket->entries.begin();it!=bucket->entries.end();) {
                auto placement=it->second.placement;
                // Never trust a pending hint: its entity CAS may still fail.
                if(it->second.pending || placement.empty() || verify) {
                    auto record=db.get(entity_record_key(EntityId(it->second.id)));
                    if(record)placement=decode_entity_record_view(record->value).placement;
                }
                if(!placement.empty() && placement!=this_node)it=bucket->entries.erase(it);else ++it;
            }
        },complete)){complete(std::make_exception_ptr(std::runtime_error("timer index queue pressure")));return false;}
        return true;
    }
    void finish(const std::shared_ptr<Watch>& watch,std::uint64_t revision,std::int64_t next,bool failure) {
        if(stopped)return;
        --counters.pending;watch->busy=false;if(watch->removed)return;
        if(failure)++counters.failures;
        due(watch,watch->revision!=revision?host->now_ms():(failure?later(limits.retry):next));
    }
    void scan(const std::shared_ptr<Watch>& watch) {
        watch->busy=true;++counters.pending;auto revision=watch->revision;auto entry=watch->entry;
        auto result=std::make_shared<Scan>();auto time=host->now_ms();result->next=later(limits.refresh);
        auto this_owner=owner,this_node=node;
        auto complete=[weak=weak_from_this(),watch,result,revision](std::exception_ptr error){
            auto self=weak.lock();if(!self || self->stopped)return;
            if(error || watch->removed)return self->finish(watch,revision,result->next,static_cast<bool>(error));
            auto done=[weak,watch,result,revision](Result value){
                auto self=weak.lock();if(!self || self->stopped)return;
                if(value.status==Status::ok && value.data=="1" && result->action==Scan::Action::fire)++self->counters.fired;
                self->finish(watch,revision,self->host->now_ms(),value.status!=Status::ok);
            };
            if(result->action==Scan::Action::wake)return self->host->read_attributes(watch->entry.type,watch->entry.id,std::move(done));
            if(result->action==Scan::Action::fire) {
                const auto& timer=*result->timer;
                return self->host->fire_timer(watch->entry.type,watch->entry.id,timer.key,timer.generation,timer.due_ms,std::move(done));
            }
            self->finish(watch,revision,result->next,false);
        };
        if(!storage.submit(5*1024*1024,[entry,result,time,this_owner,this_node](Storage& db){
            auto index=timer_index::shard(entry.id);auto bucket=timer_index::read(db,index);auto key=timer_index::identity(entry.type,entry.id);
            auto indexed=bucket.entries.find(key);if(indexed==bucket.entries.end()){result->next=INT64_MAX;return;}
            const auto observed=indexed->second;
            auto stored=db.get(entity_record_key(EntityId(entry.id)));
            if(!stored){bucket.entries.erase(indexed);result->next=timer_index::write(db,index,bucket)?INT64_MAX:time;return;}
            auto record=decode_entity_record_view(stored->value);
            if(!record.metadata || record.metadata->type!=entry.type)throw std::runtime_error("timer entity metadata mismatch");
            if(record.metadata->destroyed){bucket.entries.erase(indexed);result->next=timer_index::write(db,index,bucket)?INT64_MAX:time;return;}
            if((!record.placement.empty() && record.placement!=this_node) || (record.expires_ms>time && record.owner!=this_owner))return;
            auto timers=decode_snapshot_timers(record.snapshot);
            if(observed.pending && timers.token!=observed.token && stored->version==observed.base_version) {
                // The producer may still commit. A serialized snapshot on this
                // owner advances/fences an abandoned version after a crash.
                result->action=Scan::Action::wake;return;
            }
            if(timers.entries.empty()){bucket.entries.erase(indexed);result->next=timer_index::write(db,index,bucket)?INT64_MAX:time;return;}
            auto due=next_entity_timer(timers);
            timer_index::Entry confirmed{entry.type,entry.id,timers.token,stored->version,record.fence,due,false,std::string(record.placement)};
            if(indexed->second!=confirmed){indexed->second=confirmed;timer_index::write(db,index,bucket);}
            result->next=due;
            if(due>time)return;
            const EntityTimer* earliest=nullptr;
            for(const auto& [name,timer]:timers.entries){(void)name;if(timer.due_ms<=time && (!earliest || timer.due_ms<earliest->due_ms))earliest=&timer;}
            if(earliest){result->timer=*earliest;result->action=Scan::Action::fire;}
        },complete))complete(std::make_exception_ptr(std::runtime_error("timer scan queue pressure")));
    }
};
EntityTimers::EntityTimers(std::shared_ptr<EntityScriptHost> host,std::shared_ptr<Storage> db,EntityTimerLimits limits):impl_(std::make_shared<Impl>(std::move(host),std::move(db),limits)){}
EntityTimers::~EntityTimers(){stop();}
std::size_t EntityTimers::poll(std::size_t budget) {
    auto keep=impl_;auto& self=*keep;if(self.stopped)return 0;if(self.polling)throw std::logic_error("recursive timer poll");
    self.polling=true;struct Reset{bool& flag;~Reset(){flag=false;}}reset{self.polling};auto work=self.storage.poll(budget);
    if(self.stopped)return work;
    if(self.observed_revision!=self.host->timer_revision()){
        self.observed_revision=self.host->timer_revision();self.idle=self.limits.refresh;self.refresh=Impl::Clock::now();
    }
    if(!self.remaining && Impl::Clock::now()>=self.refresh){self.remaining=timer_index::shards;self.verify_hints=(self.rounds++%16)==0;}
    auto now=self.host->now_ms();auto maximum=self.limits.pending-(self.remaining?1:0);
    while(work<budget && self.counters.pending<maximum && !self.schedule.empty() && self.schedule.begin()->first.first<=now) {
        auto watch=self.schedule.begin()->second;self.schedule.erase(self.schedule.begin());
        if(!self.host->schema(watch->entry.type)){self.due(watch,self.later(self.limits.refresh));++work;continue;}
        self.scan(watch);++work;
    }
    while(work<budget && self.remaining && self.counters.pending<self.limits.pending) {
        if(!self.load(self.cursor%timer_index::shards))break;
        ++self.cursor;--self.remaining;++work;
        if(!self.remaining){
            self.refresh=Impl::Clock::now()+self.idle;
            if(self.watches.empty())self.idle=std::min(self.idle*2,self.limits.refresh*16);else self.idle=self.limits.refresh;
        }
    }
    return work;
}
EntityTimerStats EntityTimers::stats()const{auto result=impl_->counters;result.tracked=impl_->watches.size();return result;}
void EntityTimers::stop(){auto self=impl_;if(std::exchange(self->stopped,true))return;self->storage.stop();self->watches.clear();self->schedule.clear();self->counters.pending=0;}
}

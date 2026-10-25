#include "fsnet/entity_timers.hpp"
#include "fsnet/entity_message.hpp"
#include "fsnet/lua_engine.hpp"
#include "../src/entity_timer_index.hpp"
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <source_location>
#include <thread>
#include <unordered_map>
using namespace fsnet;using namespace std::chrono_literals;
#define CHECK(c) do{if(!(c))throw std::runtime_error(std::string(__FILE__)+":"+std::to_string(__LINE__)+" " #c);}while(false)
struct Host:ScriptHost {
    std::unordered_map<std::string,Handler> handlers;std::unordered_map<std::string,ContextHandler> contexts;
    bool has_route(const std::string& name)const override{return handlers.contains(name)||contexts.contains(name);}
    void bind(std::string name,Handler fn)override{CHECK(!has_route(name));handlers.emplace(std::move(name),std::move(fn));}
    void bind_context(std::string name,RpcPolicy,ContextHandler fn)override{CHECK(!has_route(name));contexts.emplace(std::move(name),std::move(fn));}
    void unbind(const std::string& name)override{handlers.erase(name);contexts.erase(name);}
    std::uint64_t connect(const std::string&,std::uint16_t)override{return 1;}
    void call(std::uint64_t,std::string,std::string,std::chrono::milliseconds,Reply reply)override{reply({Status::not_found,{}});}
};
struct FaultStorage:Storage {
    std::shared_ptr<Storage> parent=memory_database();
    enum Fault{none,index_ack,entity_ack};std::atomic<Fault> fault=none;
    std::optional<Record> get(std::string_view key)override{return parent->get(key);}
    bool erase(std::string_view key,std::uint64_t version)override{return parent->erase(key,version);}
    bool compare_exchange(std::string_view key,std::uint64_t version,std::string_view value)override {
        auto accepted=parent->compare_exchange(key,version,value);if(!accepted)return false;
        auto current=fault.load();
        bool inject=(current==index_ack && key.starts_with("fsnet/timers/index/") && !value.empty() && value.back()==1) ||
                    (current==entity_ack && key.starts_with("fsnet/entity/v2/"));
        if(inject && fault.exchange(none)!=none)throw std::runtime_error("injected committed write with lost ACK");
        return true;
    }
};
struct Machine {
    std::shared_ptr<Storage> db;std::shared_ptr<std::atomic<std::int64_t>> time;
    std::shared_ptr<Host> parent=std::make_shared<Host>();std::shared_ptr<EntityScriptHost> host;std::unique_ptr<LuaEngine> engine;std::unique_ptr<EntityTimers> timers;
    Machine(std::shared_ptr<Storage> database,std::shared_ptr<std::atomic<std::int64_t>> clock,const std::string& source,const std::string& boot):db(std::move(database)),time(std::move(clock)) {
        EntityRuntimeLimits limits;limits.lease=2000ms;limits.request_timeout=500ms;
        host=std::make_shared<EntityScriptHost>(parent,db,"timers/"+boot,limits,StorageLimits{},[clock=time]{return clock->load();});
        engine=std::make_unique<LuaEngine>(host);engine->load(source,"timer-fixture");EntityTimerLimits scheduling;scheduling.refresh=2ms;scheduling.retry=2ms;
        timers=std::make_unique<EntityTimers>(host,db,scheduling);
    }
    ~Machine(){timers->stop();engine->stop();host->stop();}
    void pump(){host->poll();engine->poll();timers->poll();}
    template<class F> void until(F done,std::source_location where=std::source_location::current()){auto end=std::chrono::steady_clock::now()+5s;while(!done()){pump();if(std::chrono::steady_clock::now()>end)throw std::runtime_error("timer deadline at "+std::to_string(where.line())+", pending="+std::to_string(host->stats().pending)+", tracked="+std::to_string(timers->stats().tracked)+", scans="+std::to_string(timers->stats().pending)+", failures="+std::to_string(timers->stats().failures)+": "+engine->last_error());std::this_thread::sleep_for(1ms);}}
    template<class F> Result wait(F start){std::optional<Result> result;start([&](Result value){result=std::move(value);});until([&]{return result.has_value();});return *result;}
    EntityId create(){auto result=wait([&](Reply reply){host->create_entity("TimerActor",{},{"timers","actor",{},"timers"},std::move(reply));});CHECK(result.status==Status::ok);return EntityId(result.data);}
    Result call(EntityId id,std::string method,std::string data){return wait([&](Reply reply){parent->contexts.at("@")({Caller::server,"fixture"},entity_call_message(id,method,data),std::move(reply));});}
    EntityRecord record(EntityId id){return decode_entity_record(db->get(entity_record_key(id))->value);}
    std::int64_t count(EntityId id){return std::get<std::int64_t>(decode_attributes(record(id).snapshot.state).values.at("count"));}
    void spin(unsigned rounds=20){while(rounds--){pump();std::this_thread::sleep_for(1ms);}}
};
template<class F> void denied(F function){bool failed=false;try{function();}catch(...){failed=true;}CHECK(failed);}
int main(int argc,char** argv){try{
    CHECK(argc==2);std::ifstream file(argv[1]);CHECK(file);std::string source{std::istreambuf_iterator<char>(file),{}};
    EntityTimer original{"key","callback","bytes","1.1",100,10,3};auto coalesce=original;CHECK(advance_entity_timer(coalesce,135) && coalesce.due_ms==140 && coalesce.remaining==2);
    auto catchup=original;catchup.missed=TimerMissed::catch_up;CHECK(advance_entity_timer(catchup,135) && catchup.due_ms==110);
    auto delay=original;delay.fixed_delay=true;CHECK(advance_entity_timer(delay,135) && delay.due_ms==145);
    EntityTimerState state{1,"token",{{"key",original}}};auto encoded=encode_entity_timers(state);CHECK(decode_entity_timers(encoded).entries.at("key").payload=="bytes");
    CHECK(entity_timers_wire_size(state)==encoded.size());CHECK(entity_timers_wire_size({})==encode_entity_timers({}).size());
    for(std::size_t i=0;i<encoded.size();++i)denied([&]{decode_entity_timers(std::string_view(encoded).substr(0,i));});
    denied([&]{advance_entity_timer(original,99);});
    auto time=std::make_shared<std::atomic<std::int64_t>>(10000);auto database=memory_database();
    {
        Machine machine(database,time,source,"one");auto id=machine.create();
        CHECK(machine.call(id,"arm","once").status==Status::ok);time->fetch_add(99);machine.spin();CHECK(machine.count(id)==0);
        time->fetch_add(1);machine.until([&]{return machine.count(id)==3;});machine.spin();CHECK(machine.count(id)==3);
        CHECK(machine.call(id,"arm","period").status==Status::ok);
        for(int expected=4;expected<=6;++expected){time->fetch_add(100);machine.until([&]{return machine.count(id)==expected;});}
        CHECK(!machine.record(id).snapshot.timers.entries.contains("period"));
        CHECK(machine.call(id,"arm","catchup").status==Status::ok);time->fetch_add(550);machine.until([&]{return machine.count(id)==10;});
        CHECK(machine.call(id,"arm","coalesce").status==Status::ok);time->fetch_add(550);machine.until([&]{return machine.count(id)==11;});machine.spin();CHECK(machine.count(id)==11);
        auto repeat=machine.record(id).snapshot.timers.entries.at("repeat");CHECK(repeat.remaining==3 && repeat.due_ms>time->load());
        time->store(repeat.due_ms);machine.until([&]{return machine.count(id)==12;});
        for(int expected=13;expected<=14;++expected){time->fetch_add(100);machine.until([&]{return machine.count(id)==expected;});}
        CHECK(machine.call(id,"arm","delay").status==Status::ok);time->fetch_add(250);machine.until([&]{return machine.count(id)==15;});CHECK(machine.record(id).snapshot.timers.entries.at("delay").due_ms==time->load()+100);
        time->fetch_add(100);machine.until([&]{return machine.count(id)==16;});
        CHECK(machine.call(id,"arm","cancel").status==Status::ok);time->fetch_add(200);machine.spin();CHECK(machine.count(id)==16);
        CHECK(machine.call(id,"arm","old").status==Status::ok);auto old=machine.record(id).snapshot.timers.entries.at("replace");
        CHECK(machine.call(id,"arm","replace").status==Status::ok);time->fetch_add(100);
        CHECK(machine.wait([&](Reply reply){machine.host->fire_timer("TimerActor",id.str(),old.key,old.generation,old.due_ms,std::move(reply));}).status==Status::ok);CHECK(machine.count(id)==16);
        time->fetch_add(100);machine.until([&]{return machine.count(id)==18;});
        CHECK(machine.call(id,"arm","fail").status==Status::ok);auto failures=machine.timers->stats().failures;time->fetch_add(100);
        machine.until([&]{return machine.timers->stats().failures>failures;});CHECK(machine.count(id)==18);machine.until([&]{return machine.count(id)==23;});
        CHECK(machine.call(id,"arm","rollback").status==Status::internal);CHECK(!machine.record(id).snapshot.timers.entries.contains("rollback"));
        CHECK(machine.call(id,"arm","self_cancel").status==Status::ok);time->fetch_add(500);machine.until([&]{return machine.count(id)==24;});machine.spin();CHECK(machine.count(id)==24);
        CHECK(machine.call(id,"arm","renew").status==Status::ok);time->fetch_add(100);machine.until([&]{return machine.count(id)==25;});time->fetch_add(100);machine.until([&]{return machine.count(id)==27;});
        CHECK(machine.call(id,"arm","once").status==Status::ok);auto handle=machine.host->handle("TimerActor",id.str());CHECK(handle);
        CHECK(machine.wait([&](Reply reply){machine.host->deactivate(*handle,std::move(reply));}).status==Status::ok);time->fetch_add(100);machine.until([&]{return machine.count(id)==30;});
        CHECK(machine.call(id,"arm","quota").status==Status::ok);CHECK(machine.record(id).snapshot.timers.entries.size()==64 && !machine.record(id).snapshot.timers.entries.contains("overflow"));
        CHECK(machine.wait([&](Reply reply){machine.host->control_entity("TimerActor",id.str(),true,std::move(reply));}).status==Status::ok);
        time->fetch_add(2000000);machine.spin(50);CHECK(machine.record(id).metadata->destroyed && machine.record(id).snapshot.timers.entries.empty());
    }
    for(auto fault:{FaultStorage::index_ack,FaultStorage::entity_ack}) {
        auto faulty=std::make_shared<FaultStorage>();time->store(10000);Machine machine(faulty,time,source,"fault");auto id=machine.create();
        CHECK(machine.call(id,"arm","once").status==Status::ok);faulty->fault=fault;time->fetch_add(100);
        machine.until([&]{return machine.count(id)==3 && faulty->fault==FaultStorage::none;});machine.spin(50);
        CHECK(machine.count(id)==3 && machine.record(id).snapshot.timers.entries.empty() && machine.timers->stats().failures>0);
    }
    // Foreign/unknown types must not consume a local scheduling slot, even
    // when they appear in earlier discovery shards.
    {
        time->store(10000);Machine machine(memory_database(),time,source,"capacity");auto id=machine.create();
        CHECK(machine.call(id,"arm","once").status==Status::ok);machine.timers->stop();
        for(bool unknown:{false,true}) {
            std::uint64_t number=unknown?2000000:1000000;
            while(timer_index::shard(EntityId(number).str())!=0)++number;
            auto copy=machine.record(id);copy.placement=unknown?"timers":"foreign";copy.owner=copy.placement+"/fixture";
            if(unknown)copy.metadata->type="Unknown";
            auto key=entity_record_key(EntityId(number));CHECK(machine.db->compare_exchange(key,0,encode_entity_record(copy)));
            auto bucket=timer_index::read(*machine.db,0);auto record=machine.db->get(key);
            timer_index::Entry entry{copy.metadata->type,EntityId(number).str(),copy.snapshot.timers.token,record->version,copy.fence,10100,false,copy.placement};
            bucket.entries.emplace(timer_index::identity(entry.type,entry.id),entry);CHECK(timer_index::write(*machine.db,0,bucket));
        }
        EntityTimerLimits scheduling;scheduling.tracked=1;scheduling.refresh=2ms;scheduling.retry=2ms;
        machine.timers=std::make_unique<EntityTimers>(machine.host,machine.db,scheduling);
        time->fetch_add(100);machine.until([&]{return machine.count(id)==3 && machine.timers->stats().fired==1;});
    }
    // Reopen a durable backend with no resident entity and a new process owner.
    auto directory=std::filesystem::temp_directory_path()/("fsnet-timer-restart-"+ConnectionRef::create().id);
    struct Cleanup{std::filesystem::path path;~Cleanup(){std::filesystem::remove_all(path);}} cleanup{directory};
    auto storage=file_storage(directory.string());time->store(10000);std::string saved;
    {Machine first(storage,time,source,"before");auto id=first.create();saved=id.str();CHECK(first.call(id,"arm","once").status==Status::ok);}
    time->store(15000);
    {Machine after(file_storage(directory.string()),time,source,"after");auto id=EntityId(saved);after.until([&]{return after.count(id)==3;});after.spin(50);CHECK(after.count(id)==3 && after.record(id).snapshot.timers.entries.empty());}
    std::cout<<"PASS persistent timers: codec, delay, periodic policies, finite count, replace, cancel, callback rollback/retry, idle wake and durable restart\n";
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}

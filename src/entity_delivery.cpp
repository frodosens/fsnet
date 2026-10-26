#include "fsnet/entity_delivery.hpp"
#include "fsnet/replication_wire.hpp"
#include <algorithm>
#include <unordered_map>
#include <utility>
namespace fsnet {
namespace {
using Clock=std::chrono::steady_clock;
constexpr const char* receive_route="system.delivery.receive";
constexpr const char* ack_route="system.delivery.ack";
std::int64_t wall(){return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();}
}
struct EntityDelivery::Impl:std::enable_shared_from_this<Impl> {
    struct Attempt { std::string key;DeliveryEnvelope envelope;bool done=false; };
    Node& node;std::shared_ptr<EntityScriptHost> host;AsyncDatabase storage;
    std::vector<EntityEndpoint> endpoints;std::vector<std::uint64_t> peers;
    Trust trust;EntityDeliveryLimits limits;EntityDeliveryStats counters;
    std::vector<std::pair<std::string,std::string>> sources;
    std::unordered_map<std::string,std::shared_ptr<Attempt>> active;
    std::size_t cursor=0,remaining=0;Clock::time_point next{},refresh{};bool stopped=false,loading=false,polling=false;
    Impl(Node& n,std::shared_ptr<EntityScriptHost> h,std::shared_ptr<Storage> db,std::vector<EntityEndpoint> e,Trust t,EntityDeliveryLimits l)
        :node(n),host(std::move(h)),storage(std::move(db)),endpoints(std::move(e)),trust(std::move(t)),limits(l) {
        validate_entity_endpoints(endpoints);
        if(!host||!trust||!limits.requests||limits.requests>1024||limits.retry.count()<=0||limits.refresh.count()<=0||limits.timeout.count()<=0||limits.timeout>std::chrono::minutes(5))throw std::invalid_argument("invalid delivery settings");
        peers.resize(endpoints.size());
    }
    void finish(const std::shared_ptr<Attempt>& op,bool success=false){if(std::exchange(op->done,true))return;active.erase(op->key);if(success){if(!op->envelope.message.ticket.empty())++counters.acknowledged;}else ++counters.failures;}
    void send(const std::shared_ptr<Attempt>& op,const std::string& type,const std::string& id,std::string route,std::string payload,Reply reply) {
        auto location=std::make_shared<std::optional<EntityLocation>>();
        if(!storage.submit(type.size()+id.size(),[location,type,id](Storage& db){*location=entity_location(db,type,id);},
            [weak=weak_from_this(),location,op,type,id,route=std::move(route),payload=std::move(payload),reply=std::move(reply)](std::exception_ptr error) mutable {
                auto self=weak.lock();if(!self||self->stopped||op->done)return;
                if(error)return self->finish(op);
                auto index=select_entity_endpoint(self->endpoints,type,id,*location,wall());if(!index)return self->finish(op);
                try{
                    auto& peer=self->peers[*index];const auto& endpoint=self->endpoints[*index];
                    if(!peer||(!self->node.connected(peer)&&!self->node.connecting(peer)))peer=self->node.connect(endpoint.address,endpoint.port,endpoint.tls_peer_name);
                    self->node.call(peer,route,payload,self->limits.timeout,std::move(reply));
                }catch(...){self->finish(op);}
            }))finish(op);
    }
    void deliver(const std::shared_ptr<Attempt>& op) {
        ++counters.attempts;const auto& m=op->envelope.message;
        send(op,m.type,m.id,receive_route,encode_delivery(op->envelope),[weak=weak_from_this(),op](Result result) {
            auto self=weak.lock();if(!self||self->stopped||op->done)return;
            if(result.status!=Status::ok)return self->finish(op);
            try{
                auto [ticket,value]=decode_delivery_ack(result.data);if(ticket!=op->envelope.message.ticket)throw std::invalid_argument("ACK identity mismatch");
                auto payload=encode_entity_key(op->envelope.source_type,op->envelope.source_id);
                entity_record_detail::integer(payload,op->envelope.source_epoch);snapshot_detail::blob(payload,result.data);
                self->send(op,op->envelope.source_type,op->envelope.source_id,ack_route,std::move(payload),[weak,op](Result result){if(auto self=weak.lock();self&&!self->stopped&&!op->done)self->finish(op,result.status==Status::ok);});
            }catch(...){self->finish(op);}
        });
    }
    void scan(const std::pair<std::string,std::string>& source) {
        auto key=encode_entity_key(source.first,source.second);if(active.contains(key))return;
        auto op=std::make_shared<Attempt>();op->key=key;active.emplace(key,op);
        auto envelope=std::make_shared<std::optional<DeliveryEnvelope>>();
        auto retired=std::make_shared<bool>(false);
        if(!storage.submit(key.size(),[source,envelope,retired](Storage& db){
            auto record=db.get(entity_record_key(EntityId(source.second)));if(!record)return;
            auto entity=decode_entity_record(record->value);
            auto& state=entity.snapshot.delivery;
            if(entity.metadata && entity.metadata->destroyed) {
                // Only permanent global tombstones can retire a registration.
                // A live empty outbox may be between registration and its source
                // CAS; removing it could make an accepted message undiscoverable.
                if(entity.metadata->type!=source.first ||
                    std::any_of(state.outbox.begin(),state.outbox.end(),[](const auto& m){return !m.done;}))return;
                for(unsigned attempt=0;attempt<8;++attempt) {
                    auto registry=db.get(delivery_registry_key);if(!registry)return;
                    auto sources=decode_delivery_sources(registry->value);
                    auto found=std::find(sources.begin(),sources.end(),source);
                    if(found==sources.end()){*retired=true;return;}
                    sources.erase(found);
                    if(db.compare_exchange(delivery_registry_key,registry->version,encode_delivery_sources(sources))) {
                        *retired=true;return;
                    }
                }
                return; // A competing registration wins; retry a later sweep.
            }
            for(auto& m:state.outbox)if(!m.done){*envelope=DeliveryEnvelope{source.first,source.second,state.epoch,std::move(m)};break;}
        },[weak=weak_from_this(),op,envelope,retired](std::exception_ptr error){
            auto self=weak.lock();if(!self||self->stopped||op->done)return;
            if(error)return self->finish(op);
            if(*retired)self->refresh={};
            if(!*envelope)return self->finish(op,true);
            op->envelope=std::move(**envelope);self->deliver(op);
        }))finish(op);
    }
    void load(){
        loading=true;auto value=std::make_shared<std::vector<std::pair<std::string,std::string>>>();
        if(!storage.submit(1024,[value](Storage& db){if(auto record=db.get(delivery_registry_key))*value=decode_delivery_sources(record->value);},
            [weak=weak_from_this(),value](std::exception_ptr error){auto self=weak.lock();if(!self||self->stopped)return;self->loading=false;self->refresh=Clock::now()+self->limits.refresh;if(!error){self->sources=std::move(*value);self->cursor=self->sources.empty()?0:self->cursor%self->sources.size();self->remaining=std::min(self->remaining,self->sources.size());}else ++self->counters.failures;})) {loading=false;refresh=Clock::now()+limits.refresh;++counters.failures;}
    }
};
EntityDelivery::EntityDelivery(Node& node,std::shared_ptr<EntityScriptHost> host,std::shared_ptr<Storage> database,std::vector<EntityEndpoint> endpoints,Trust trust,EntityDeliveryLimits limits)
    :impl_(std::make_shared<Impl>(node,std::move(host),std::move(database),std::move(endpoints),std::move(trust),limits)) {
    if(node.has_route(receive_route)||node.has_route(ack_route))throw std::logic_error("delivery routes already bound");
    auto weak=std::weak_ptr(impl_);
    try{
        node.bind_rpc(receive_route,{false,true,false},[weak](const CallIdentity& who,std::string data,Reply reply){
            auto self=weak.lock();if(!self||self->stopped)return reply({Status::disconnected,{}});
            if(!self->trust(who))return reply({Status::rejected,{}});
            try{auto envelope=decode_delivery(data);self->host->receive_delivery(who,std::move(envelope),std::move(reply));}catch(...){if(reply)reply({Status::rejected,{}});}
        });
        node.bind_rpc(ack_route,{false,true,false},[weak](const CallIdentity& who,std::string data,Reply reply){
            auto self=weak.lock();if(!self||self->stopped)return reply({Status::disconnected,{}});
            if(!self->trust(who))return reply({Status::rejected,{}});
            try{
                std::string_view input(data);auto [type,id]=decode_entity_key(input);auto epoch=entity_record_detail::integer(input);auto ack=snapshot_detail::blob(input);if(!input.empty())throw std::invalid_argument("ACK trailing bytes");
                auto [ticket,value]=decode_delivery_ack(ack);self->host->acknowledge_delivery(std::move(type),std::move(id),epoch,std::move(ticket),std::move(value),std::move(reply));
            }catch(...){if(reply)reply({Status::rejected,{}});}
        });
        impl_->host->enable_delivery(true);
    }catch(...){node.unbind(receive_route);node.unbind(ack_route);throw;}
}
EntityDelivery::~EntityDelivery(){stop();}
void EntityDelivery::poll(std::size_t budget){
    auto keep=impl_;auto& self=*keep;if(self.stopped)return;
    if(self.polling)throw std::logic_error("recursive delivery poll");
    self.polling=true;struct Reset{bool& value;~Reset(){value=false;}}reset{self.polling};
    auto work=self.storage.poll(budget);auto now=Clock::now();
    if(self.stopped)return;
    if(!self.loading&&now>=self.refresh)self.load();
    if(self.sources.empty())return;
    // Rate-limit complete sweeps, not individual request-sized chunks. A large
    // mostly empty registry must not wait retry between every batch of reads.
    // Async completion and the caller's budget still bound each poll's work.
    if(!self.remaining){if(now<self.next)return;self.remaining=self.sources.size();}
    auto count=std::min(self.remaining,budget>work?budget-work:0);
    while(count--&&self.active.size()<self.limits.requests){const auto source=self.sources[self.cursor++%self.sources.size()];--self.remaining;self.scan(source);}
    if(!self.remaining)self.next=now+self.limits.retry;
}
EntityDeliveryStats EntityDelivery::stats() const{auto value=impl_->counters;value.sources=impl_->sources.size();value.pending=impl_->active.size();return value;}
void EntityDelivery::stop(){auto keep=impl_;auto& self=*keep;if(std::exchange(self.stopped,true))return;self.host->enable_delivery(false);self.node.unbind(receive_route);self.node.unbind(ack_route);for(auto& [key,op]:self.active){(void)key;op->done=true;}self.active.clear();for(auto peer:self.peers)if(peer)self.node.close(peer);self.storage.stop(AsyncDatabase::StopMode::cancel_queued);}
}

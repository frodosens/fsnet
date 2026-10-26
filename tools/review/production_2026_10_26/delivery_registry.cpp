#include "support/runtime.hpp"
#include "fsnet/entity_delivery.hpp"
#include <atomic>
#include <cstdlib>
#include <iostream>
using namespace fsnet;using namespace testing;
const char* delivery_source=R"lua(
Receiver=fsnet.Entity:extend('Receiver')
Receiver:attr('gold',{type='int32',default=0})
Receiver:rpc('grant',{callers={'server'}})
function Receiver:grant(value) self.gold=self.gold+assert(tonumber(value));return tostring(self.gold) end
Receiver:register()
Source=fsnet.Entity:extend('Source')
Source:attr('ticket',{type='string',default=''})
Source:rpc('queue',{callers={'server'}})
function Source:queue(id) self.ticket=Receiver:ref(id):send('grant','3');return self.ticket end
Source:rpc('fail',{callers={'server'}})
function Source:fail(id) self.ticket=Receiver:ref(id):send('grant','100');error('rollback') end
Source:register()
)lua";
struct FaultStorage:Storage {
    std::shared_ptr<Storage> backend;std::atomic<bool> crash=false;
    explicit FaultStorage(std::shared_ptr<Storage> storage):backend(std::move(storage)){}
    std::optional<Record> get(std::string_view key)override{return backend->get(key);}
    bool erase(std::string_view key,std::uint64_t version)override{return backend->erase(key,version);}
    bool compare_exchange(std::string_view key,std::uint64_t version,std::string_view bytes)override{
        auto accepted=backend->compare_exchange(key,version,bytes);
        if(accepted && crash && key.starts_with("fsnet/entity/v2/")){
            auto record=decode_entity_record(bytes);
            if(record.metadata->type=="Receiver" && !record.snapshot.delivery.inbox.empty())std::_Exit(42);
        }
        return accepted;
    }
};
struct DeliveryRuntime {
    Node node{"current-delivery"};std::shared_ptr<FaultStorage> db;std::shared_ptr<EntityScriptHost> host;
    std::unique_ptr<LuaEngine> vm;std::unique_ptr<EntityDelivery> service;
    DeliveryRuntime(std::shared_ptr<Storage> storage):db(std::make_shared<FaultStorage>(std::move(storage))){
        node.identify([](auto,const auto&)->std::optional<CallIdentity>{return CallIdentity{Caller::server,"backend"};});
        EntityRuntimeLimits limits;limits.delivery_sources=2;limits.lease=500ms;limits.request_timeout=200ms;
        host=std::make_shared<EntityScriptHost>(std::make_shared<NodeScriptHost>(node),db,"delivery/"+ConnectionRef::create().id,limits);
        vm=std::make_unique<LuaEngine>(host);vm->load(delivery_source,"current-delivery");
        auto port=node.listen("127.0.0.1",0);EntityDeliveryLimits delivery;delivery.retry=5ms;delivery.refresh=5ms;
        service=std::make_unique<EntityDelivery>(node,host,db,std::vector<EntityEndpoint>{{"delivery","127.0.0.1",port,{"Source","Receiver"}}},
            [](const CallIdentity& who){return who.caller==Caller::server && who.subject=="backend";},delivery);
    }
    ~DeliveryRuntime(){service->stop();vm->stop();host->stop();node.stop();}
    void poll(){node.poll();host->poll();vm->poll();service->poll();}
    template<class F> void until(F done){auto end=std::chrono::steady_clock::now()+8s;while(!done()){poll();require(std::chrono::steady_clock::now()<end,"delivery deadline");std::this_thread::sleep_for(1ms);}}
    template<class F> Result wait(F begin){std::optional<Result> result;begin([&](Result value){result=std::move(value);});until([&]{return result.has_value();});return *result;}
    EntityId create(std::string type,std::string key={}){if(key.empty())key=type;auto value=wait([&](auto reply){host->create_entity(type,{},{"delivery-current",key,{},"delivery"},std::move(reply));});require(value.status==Status::ok,"delivery entity creation");return EntityId(value.data);}
    Result call(EntityId id,std::string_view method,std::string_view value){return wait([&](auto reply){host->call_entity("@",entity_call_message(id,method,value),2s,std::move(reply));});}
    EntityRecord record(EntityId id){return decode_entity_record(db->get(entity_record_key(id))->value);}
    std::int64_t gold(EntityId id){return std::get<std::int64_t>(decode_attributes(record(id).snapshot.state).values.at("gold"));}
    bool complete(EntityId id){auto record=this->record(id);return !record.snapshot.delivery.outbox.empty() && record.snapshot.delivery.outbox.front().done;}
};

int main(){try {
    DeliveryRuntime runtime(memory_database());auto receiver=runtime.create("Receiver");
    for(unsigned i=0;i<2;++i){
        auto sender=runtime.create("Source","source-"+std::to_string(i));
        require(runtime.call(sender,"queue",receiver.str()).status==Status::ok,"first sources enqueue");
        runtime.until([&]{return runtime.complete(sender) && runtime.host->stats().pending==0;});
        auto dead=runtime.wait([&](auto reply){runtime.host->control_entity("Source",sender.str(),true,std::move(reply));});
        require(dead.status==Status::ok,"destroy completed source");
    }
    auto saved=runtime.db->get(delivery_registry_key);auto sources=decode_delivery_sources(saved->value);
    auto next=runtime.create("Source","source-next");auto result=runtime.call(next,"queue",receiver.str());
    std::cout<<"destroyed_sources=2 registry_sources="<<sources.size()<<" new_send_status="<<unsigned(result.status)<<" receiver_gold="<<runtime.gold(receiver)<<" pending="<<runtime.host->stats().pending<<'\n';
    require(sources.size()==2 && result.status!=Status::ok,"registry unexpectedly reclaimed");
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}

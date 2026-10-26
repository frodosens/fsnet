#include "support/runtime.hpp"
#include "fsnet/entity_delivery.hpp"
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <mutex>
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
    bool poll_delivery=true;
    DeliveryRuntime(std::shared_ptr<Storage> storage,std::size_t source_limit=4096):db(std::make_shared<FaultStorage>(std::move(storage))){
        node.identify([](auto,const auto&)->std::optional<CallIdentity>{return CallIdentity{Caller::server,"backend"};});
        EntityRuntimeLimits limits;limits.lease=500ms;limits.request_timeout=200ms;limits.delivery_sources=source_limit;
        host=std::make_shared<EntityScriptHost>(std::make_shared<NodeScriptHost>(node),db,"delivery/"+ConnectionRef::create().id,limits);
        vm=std::make_unique<LuaEngine>(host);vm->load(delivery_source,"current-delivery");
        auto port=node.listen("127.0.0.1",0);EntityDeliveryLimits delivery;delivery.retry=5ms;delivery.refresh=5ms;
        service=std::make_unique<EntityDelivery>(node,host,db,std::vector<EntityEndpoint>{{"delivery","127.0.0.1",port,{"Source","Receiver"}}},
            [](const CallIdentity& who){return who.caller==Caller::server && who.subject=="backend";},delivery);
    }
    ~DeliveryRuntime(){service->stop();vm->stop();host->stop();node.stop();}
    void poll(){node.poll();host->poll();vm->poll();if(poll_delivery)service->poll();}
    template<class F> void until(F done){auto end=std::chrono::steady_clock::now()+8s;while(!done()){poll();require(std::chrono::steady_clock::now()<end,"delivery deadline");std::this_thread::sleep_for(1ms);}}
    template<class F> Result wait(F begin){std::optional<Result> result;begin([&](Result value){result=std::move(value);});until([&]{return result.has_value();});return *result;}
    EntityId create(std::string type,std::string key={}){if(key.empty())key=type;auto value=wait([&](auto reply){host->create_entity(type,{},{"delivery-current",key,{},"delivery"},std::move(reply));});require(value.status==Status::ok,"delivery entity creation");return EntityId(value.data);}
    Result call(EntityId id,std::string_view method,std::string_view value){return wait([&](auto reply){host->call_entity("@",entity_call_message(id,method,value),2s,std::move(reply));});}
    EntityRecord record(EntityId id){return decode_entity_record(db->get(entity_record_key(id))->value);}
    std::int64_t gold(EntityId id){return std::get<std::int64_t>(decode_attributes(record(id).snapshot.state).values.at("gold"));}
    bool complete(EntityId id){auto record=this->record(id);return !record.snapshot.delivery.outbox.empty() && record.snapshot.delivery.outbox.front().done;}
};

void destroyed_source_capacity(){
    DeliveryRuntime runtime(memory_database(),2);
    auto receiver=runtime.create("Receiver"),first=runtime.create("Source","first"),second=runtime.create("Source","second");
    require(runtime.call(first,"queue",receiver.str()).status==Status::ok,"first bounded source enqueue");
    require(runtime.call(second,"queue",receiver.str()).status==Status::ok,"second bounded source enqueue");
    runtime.until([&]{return runtime.complete(first) && runtime.complete(second) && runtime.host->stats().pending==0 && runtime.service->stats().pending==0;});
    runtime.poll_delivery=false;
    for(auto id:{first,second})require(runtime.wait([&](auto reply){runtime.host->control_entity("Source",id.str(),true,std::move(reply));}).status==Status::ok,"destroy completed bounded source");
    auto registry=[&]{return decode_delivery_sources(runtime.db->get(delivery_registry_key)->value);};
    require(registry().size()==2,"retirement waits for background polling");
    auto replacement=runtime.create("Source","replacement");
    auto indexed=registry();
    // Lua send raises on rejected admission, so the enclosing business RPC may
    // report internal rather than preserving the storage admission status.
    require(runtime.call(replacement,"queue",receiver.str()).status!=Status::ok,"unreclaimed source capacity rejects the new send");
    require(registry()==indexed && runtime.record(replacement).snapshot.delivery.outbox.empty() && runtime.gold(receiver)==6,
        "rejected send does not change the index, outbox or receiver");
    runtime.poll_delivery=true;
    runtime.until([&]{return registry().empty();});
    require(runtime.call(replacement,"queue",receiver.str()).status==Status::ok,"destroyed source capacity becomes reusable");
    runtime.until([&]{return runtime.complete(replacement);});
    require(runtime.gold(receiver)==9,"retirement does not lose or duplicate accepted deliveries");
}

struct RegistryRaceStorage:Storage {
    std::shared_ptr<Storage> backend=memory_database();
    std::mutex mutex;
    std::optional<std::pair<std::string,std::string>> addition;
    std::string retiring;
    std::atomic<unsigned> injected=0;
    std::optional<Record> get(std::string_view key)override{return backend->get(key);}
    bool erase(std::string_view key,std::uint64_t version)override{return backend->erase(key,version);}
    void arm(std::string id,std::pair<std::string,std::string> source){std::lock_guard lock(mutex);retiring=std::move(id);addition=std::move(source);}
    bool compare_exchange(std::string_view key,std::uint64_t version,std::string_view bytes)override{
        if(key!=delivery_registry_key)return backend->compare_exchange(key,version,bytes);
        std::lock_guard lock(mutex);
        auto desired=decode_delivery_sources(bytes);
        if(addition && std::none_of(desired.begin(),desired.end(),[&](const auto& source){return source.second==retiring;})){
            // Model a second owner registering a live source between a retiring
            // worker's registry read and CAS. Its source CAS has not run yet.
            auto record=backend->get(key);require(record.has_value(),"race fixture registry exists");
            auto current=decode_delivery_sources(record->value);current.push_back(*addition);
            require(backend->compare_exchange(key,record->version,encode_delivery_sources(current)),"race fixture registration CAS");
            addition.reset();++injected;
        }
        return backend->compare_exchange(key,version,bytes);
    }
};

void concurrent_retirement(){
    auto storage=std::make_shared<RegistryRaceStorage>();
    DeliveryRuntime runtime(storage,4);
    auto receiver=runtime.create("Receiver"),dead=runtime.create("Source","dead");
    auto live=runtime.create("Source","live-before-cas"),late=runtime.create("Source","concurrent-registration");
    require(runtime.call(dead,"queue",receiver.str()).status==Status::ok,"retirement race source enqueue");
    runtime.until([&]{return runtime.complete(dead) && runtime.host->stats().pending==0 && runtime.service->stats().pending==0;});
    runtime.poll_delivery=false;
    require(runtime.wait([&](auto reply){runtime.host->control_entity("Source",dead.str(),true,std::move(reply));}).status==Status::ok,"retirement race source destroy");
    auto record=storage->get(delivery_registry_key);auto indexed=decode_delivery_sources(record->value);
    indexed.emplace_back("Source",live.str());
    require(storage->compare_exchange(delivery_registry_key,record->version,encode_delivery_sources(indexed)),"register empty live source before its business CAS");
    storage->arm(dead.str(),{"Source",late.str()});
    DeliveryRuntime other(storage,4); // independent node, host and registry scanner
    runtime.poll_delivery=true;
    auto end=std::chrono::steady_clock::now()+8s;
    auto pump=[&]{runtime.poll();other.poll();require(std::chrono::steady_clock::now()<end,"concurrent retirement deadline");std::this_thread::sleep_for(1ms);};
    auto current=[&]{return decode_delivery_sources(storage->get(delivery_registry_key)->value);};
    auto preserved=[&]{auto values=current();return values.size()==2 &&
        std::find(values.begin(),values.end(),std::pair(std::string("Source"),live.str()))!=values.end() &&
        std::find(values.begin(),values.end(),std::pair(std::string("Source"),late.str()))!=values.end();};
    while(!preserved())pump();
    auto stable=std::chrono::steady_clock::now()+30ms;
    while(std::chrono::steady_clock::now()<stable)pump();
    require(storage->injected==1 && preserved(),"retirement CAS retry preserves concurrent and empty live registrations");
    other.service->stop();
    require(runtime.call(live,"queue",receiver.str()).status==Status::ok,"pre-registered live source remains usable");
    require(runtime.call(late,"queue",receiver.str()).status==Status::ok,"concurrently registered source remains usable");
    runtime.until([&]{return runtime.complete(live) && runtime.complete(late);});
    require(runtime.gold(receiver)==9,"two scanners and registry CAS conflict preserve effects");
}

int main(int argc,char** argv){try{
    bool crash=argc==3 && std::string_view(argv[1])=="--crash";
    bool recover=argc==3 && std::string_view(argv[1])=="--recover";
    require(argc==1 || crash || recover,"delivery arguments");
    DeliveryRuntime runtime(argc==1?memory_database():open_storage(argv[2]));
    auto sender=runtime.create("Source"),receiver=runtime.create("Receiver");
    std::cout<<"entities "<<sender.str()<<' '<<receiver.str()<<std::endl;
    if(crash){runtime.db->crash=true;require(runtime.call(sender,"queue",receiver.str()).status==Status::ok,"source commit");runtime.until([]{return false;});}
    if(!recover){
        require(runtime.call(sender,"fail",receiver.str()).status!=Status::ok,"outbox rollback result");
        require(runtime.record(sender).snapshot.delivery.outbox.empty(),"failed outbox committed");
        require(runtime.call(sender,"queue",receiver.str()).status==Status::ok,"source enqueue");
    }
    runtime.until([&]{return runtime.complete(sender);});require(runtime.gold(receiver)==3,"reward lost or duplicated");
    auto saved=runtime.record(sender);DeliveryEnvelope envelope{"Source",sender.str(),saved.snapshot.delivery.epoch,saved.snapshot.delivery.outbox.front()};
    envelope.message.done=false;envelope.message.result={Status::ok,{}};
    require(runtime.wait([&](auto reply){runtime.host->receive_delivery({Caller::client,"client"},envelope,std::move(reply));}).status==Status::rejected,"client delivery accepted");
    require(runtime.wait([&](auto reply){runtime.host->receive_delivery({Caller::server,"backend"},envelope,std::move(reply));}).status==Status::ok,"delivery replay rejected");
    require(runtime.gold(receiver)==3,"duplicate delivery reexecuted");
    if(argc==1){destroyed_source_capacity();concurrent_retirement();}
    std::cout<<"PASS current delivery: global actors, rollback, committed outbox, caller roles, duplicate inbox and restart recovery\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}

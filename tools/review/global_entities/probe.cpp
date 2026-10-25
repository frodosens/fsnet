// Diagnostic only: current ownership/copy costs and timer discovery admission.
// Build/run instructions and limits are recorded in the accompanying review.
#include "fsnet/entity_timers.hpp"
#include "fsnet/entity_message.hpp"
#include "fsnet/entity_record.hpp"
#include "fsnet/attribute_wire.hpp"
#include "fsnet/database.hpp"
#include "fsnet/client_gateway.hpp"
#include "entity_timer_index.hpp"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <new>
#include <thread>
namespace {
thread_local bool tracking=false;
thread_local std::size_t allocations=0,allocated_bytes=0;
volatile std::size_t checksum=0;
}
void* operator new(std::size_t size) {
    if(tracking){++allocations;allocated_bytes+=size;}
    if(auto p=std::malloc(size?size:1))return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p)noexcept{std::free(p);}
void operator delete(void* p,std::size_t)noexcept{std::free(p);}
void operator delete[](void* p)noexcept{std::free(p);}
void operator delete[](void* p,std::size_t)noexcept{std::free(p);}
using namespace fsnet;
using namespace std::chrono_literals;
template<class F> void measure(const std::string& name,std::size_t count,F function) {
    for(unsigned i=0;i<100;++i)checksum=checksum+function();
    allocations=allocated_bytes=0;
    auto begin=std::chrono::steady_clock::now();tracking=true;
    for(std::size_t i=0;i<count;++i)checksum=checksum+function();
    tracking=false;auto end=std::chrono::steady_clock::now();
    std::cout<<"{\"probe\":\""<<name<<"\",\"iterations\":"<<count
        <<",\"ns_per_op\":"<<std::chrono::duration<double,std::nano>(end-begin).count()/count
        <<",\"allocations_per_op\":"<<double(allocations)/count
        <<",\"allocated_bytes_per_op\":"<<double(allocated_bytes)/count<<"}\n";
}
// Local comparison candidate, not a change to runtime authorization.
bool borrowed_match(const ConnectionRef& connection,std::string_view key) {
    if(!connection.valid() || !valid_connection_key(key))return false;
    key.remove_prefix(11);if(key.substr(0,32)!=connection.id)return false;
    key.remove_prefix(33);std::uint64_t generation=0;
    auto [end,error]=std::from_chars(key.data(),key.data()+key.size(),generation);
    return error==std::errc{} && end==key.data()+key.size() && generation==connection.generation;
}
struct Host:ScriptHost {
    bool has_route(const std::string&)const override{return false;}
    void bind(std::string,Handler)override{}
    void bind_context(std::string,RpcPolicy,ContextHandler)override{}
    void unbind(const std::string&)override{}
    std::uint64_t connect(const std::string&,std::uint16_t)override{return 0;}
    void call(std::uint64_t,std::string,std::string,std::chrono::milliseconds,Reply reply)override{reply({Status::not_found,{}});}
};
struct Counting:Storage {
    std::shared_ptr<Storage> parent=memory_database();
    std::atomic<std::size_t> index_reads=0,local_reads=0,foreign_reads=0;
    std::string local_key,foreign_key;
    std::optional<Record> get(std::string_view key)override{
        if(key.starts_with("fsnet/timers/index/"))++index_reads;
        if(key==local_key)++local_reads;
        if(key==foreign_key)++foreign_reads;
        return parent->get(key);
    }
    bool compare_exchange(std::string_view key,std::uint64_t version,std::string_view value)override{return parent->compare_exchange(key,version,value);}
    bool erase(std::string_view key,std::uint64_t version)override{return parent->erase(key,version);}
};
std::string id_for(unsigned shard){for(std::uint64_t i=1;;++i){auto id=EntityId(i).str();if(timer_index::shard(id)==shard)return id;}}
void timers(bool seed,std::size_t capacity) {
    auto db=std::make_shared<Counting>();auto parent=std::make_shared<Host>();
    auto host=std::make_shared<EntityScriptHost>(parent,db,"local/fixture",EntityRuntimeLimits{},StorageLimits{},[]{return 100000;});
    std::size_t callbacks=0;
    if(seed) {
        host->define_schema("Probe",{});
        auto initial=encode_attributes({0,0,true,{}});
        host->bind_stateful_context("Probe.$timer","Probe",initial,{false,true,false},
            [&](const CallIdentity&,std::string data,Reply reply){
                auto entity=parse_entity_message_view(data);if(!entity)throw std::runtime_error("probe entity envelope");
                auto state=parse_state_message_view(entity->payload);if(!state)throw std::runtime_error("probe state envelope");
                ++callbacks;reply({Status::ok,state_message(state->state,"")});
            });
        for(bool local:{false,true}) {
            auto id=id_for(local?63:0);auto key=entity_record_key(EntityId(id));
            (local?db->local_key:db->foreign_key)=key;
            EntityRecord record;record.owner=local?"local/fixture":"foreign/fixture";
            record.placement=local?"local":"foreign";record.expires_ms=200000;record.fence=1;
            record.metadata=EntityMetadata{"Probe","",std::string(32,'x'),false,false,"",1};record.snapshot.state=initial;
            record.snapshot.timers={1,"token",{{"once",{"once","callback","","1.1",1,0,1}}}};
            if(!db->compare_exchange(key,0,encode_entity_record(record)))throw std::runtime_error("probe seed entity");
            auto version=db->parent->get(key)->version;
            timer_index::Bucket bucket;timer_index::Entry entry{"Probe",id,"token",version,1,1,false};
            bucket.entries.emplace(timer_index::identity("Probe",id),entry);
            if(!timer_index::write(*db,timer_index::shard(id),bucket))throw std::runtime_error("probe seed index");
        }
    }
    db->index_reads=0;EntityTimerLimits limits;limits.tracked=capacity;
    EntityTimers service(host,db,limits);auto begin=std::chrono::steady_clock::now();
    while(std::chrono::steady_clock::now()-begin<1000ms){host->poll();service.poll();std::this_thread::sleep_for(1ms);}
    auto stats=service.stats();service.stop();host->stop();
    std::cout<<"{\"probe\":\""<<(seed?"timer_capacity":"timer_idle")<<"\",\"tracked_limit\":"<<capacity
        <<",\"duration_ms\":1000,\"index_reads\":"<<db->index_reads
        <<",\"local_entity_reads\":"<<db->local_reads<<",\"foreign_entity_reads\":"<<db->foreign_reads
        <<",\"callbacks\":"<<callbacks<<",\"fired\":"<<stats.fired
        <<",\"capacity_failures\":"<<stats.capacity_failures<<"}\n";
}
void gateway_routes() {
    auto db=memory_database();Node owner("review"),front("review"),back("review"),client("review");
    auto host=std::make_shared<EntityScriptHost>(std::make_shared<NodeScriptHost>(owner),db,"owner/fixture");
    EntitySchema schema;schema.methods.emplace("public_method",RpcPolicy{true,false,false});
    schema.methods.emplace("server_only",RpcPolicy{false,true,false});host->define_schema("Probe",schema);
    host->bind_stateful_context("Probe.public_method","Probe",encode_attributes({0,0,true,{}}),{true,false,false},
        [](const CallIdentity&,std::string data,Reply reply){
            auto entity=parse_entity_message_view(data);auto state=entity?parse_state_message_view(entity->payload):std::nullopt;
            if(!state)return reply({Status::rejected,{}});
            reply({Status::ok,state_message(state->state,"reachable")});
        });
    host->bind_stateful_context("Probe.server_only","Probe",encode_attributes({0,0,true,{}}),{false,true,false},
        [](const CallIdentity&,std::string,Reply reply){reply({Status::internal,{}});});
    auto base=[](std::uint64_t,const std::string&)->std::optional<CallIdentity>{return CallIdentity{Caller::server,"gateway"};};
    GatewayDelegation delegation(owner,base,[](const CallIdentity& who,const std::string&){return who.caller==Caller::server && who.subject=="gateway";},host);
    auto port=owner.listen("127.0.0.1",0);
    front.identify([](std::uint64_t,const std::string&)->std::optional<CallIdentity>{return CallIdentity{Caller::client,"alice"};});
    ClientGateway gateway(front,back,db,{{"owner","127.0.0.1",port,{"Probe"}}});
    gateway.bind_entity("Probe",{"allowed_method"}); // public_method is absent.
    auto peer=client.connect("127.0.0.1",front.listen("127.0.0.1",0));
    auto pump=[&]{owner.poll();front.poll();back.poll();client.poll();host->poll();delegation.poll();gateway.poll();};
    auto until=[&](auto done){auto end=std::chrono::steady_clock::now()+5s;while(!done()){pump();if(std::chrono::steady_clock::now()>end)throw std::runtime_error("gateway probe timeout");std::this_thread::sleep_for(1ms);}};
    std::optional<Result> created;
    host->create_entity("Probe",{},{"review","public",{},"owner"},[&](Result value){created=std::move(value);});
    until([&]{return created.has_value() && client.connected(peer);});
    if(created->status!=Status::ok)throw std::runtime_error("gateway probe creation failed");
    auto call=[&](std::string route,std::string data){std::optional<Result> result;client.call(peer,route,data,3s,[&](Result value){result=std::move(value);});until([&]{return result.has_value();});return *result;};
    auto typed=call("Probe.public_method",entity_message(created->data,""));
    auto global=call(entity_call_route,entity_call_message(EntityId(created->data),"public_method",""));
    auto backend=call(entity_call_route,entity_call_message(EntityId(created->data),"server_only",""));
    std::cout<<"{\"probe\":\"gateway_allowlist\",\"typed_status\":"<<unsigned(typed.status)<<",\"global_status\":"<<unsigned(global.status)
        <<",\"server_only_status\":"<<unsigned(backend.status)<<",\"global_reply\":\""<<global.data<<"\"}\n";
}
int main(){try{
    CallIdentity identity{Caller::client,"fixture",0,{std::string(32,'a'),1}};auto owner=identity.connection.key();
    measure("owns_current",500000,[&]{return identity.owns(owner);});
    measure("owns_borrowed_candidate",500000,[&]{return borrowed_match(identity.connection,owner);});
    unsigned live_calls=0;identity.current=[&]{++live_calls;return true;};
    if(!RpcPolicy{true,false,true}.allows(identity,owner))throw std::runtime_error("probe ownership failed");
    std::cout<<"{\"probe\":\"owner_policy_live_checks\",\"calls\":"<<live_calls<<"}\n";
    for(std::size_t size:{128,4096,65536}) {
        auto wire=entity_call_message(EntityId(1),"move",std::string(size,'p'));auto view=*parse_entity_call_view(wire);
        measure("router_adapter_current_"+std::to_string(size),20000,[&]{
            auto method=std::string(view.method),payload=std::string(view.payload),request=std::string(view.request_id),type=std::string(view.expected_type);
            return entity_call_message(view.id,method,payload,request,type).size();
        });
        measure("router_adapter_borrowed_reencode_"+std::to_string(size),20000,[&]{return entity_call_message(view.id,view.method,view.payload,view.request_id,view.expected_type).size();});
    }
    timers(false,65536);timers(true,1);timers(true,2);gateway_routes();
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}

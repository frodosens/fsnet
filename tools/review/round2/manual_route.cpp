#include "fsnet/client_gateway.hpp"
#include "fsnet/entity_transfer.hpp"
#include "fsnet/attribute_replication.hpp"
#include "fsnet/lua_engine.hpp"
#include "fsnet/entity_message.hpp"
#include "fsnet/entity_record.hpp"
#include "fsnet/replication_wire.hpp"
#include <iostream>
#include <source_location>
#include <thread>
#include <unordered_map>
using namespace std::chrono_literals;
#define CHECK(x) do{if(!(x))throw std::runtime_error(std::string(__FILE__)+':'+std::to_string(__LINE__)+" " #x);}while(false)
const char* model=R"(
    local Actor=fsnet.Entity:extend('Actor',{owner='id'})
    Actor:attr('position',{type='vec3',client='rw',sync='observers'})
    Actor:attr('gold',{type='int32',default=0,sync='owner'})
    Actor:attr('activations',{type='int32',default=0,sync='owner'})
    function Actor:on_activate() self.activations=self.activations+1 end
    Actor:rpc('move',{callers={'client'},access='owner'})
    function Actor:move(data) self.position.x=self.position.x+assert(tonumber(data)); self.gold=self.gold+1; return self.id end
    Actor:rpc('server_only',{callers={'server'}})
    function Actor:server_only() return 'secret' end
    function Actor:validate_write(values) return not values.position or values.position.x<=20 end
    Actor:register()
    local Pet=Actor:extend('Pet'); Pet:register()
    runtime.rpc('pet_login',{callers={'client'},handler=function(ctx) local actor=Pet:spawn_or_restore(ctx.subject); actor:show_to(ctx.peer); actor:possess(ctx.peer); return actor.id end})
    runtime.rpc('login',{callers={'client'},handler=function(ctx,name)
        local actor=Actor:spawn_or_restore(ctx.subject)
        actor:show_to(ctx.peer); actor:possess(ctx.peer); return actor.id
    end})
)";
void restored_route_expires_old_lookup(){
    auto database=fsnet::memory_database();fsnet::Node a("internal"),b("internal"),front("clients"),back("internal"),client("clients");
    fsnet::EntityRuntimeLimits runtime;runtime.lease=500ms;runtime.request_timeout=50ms;
    auto ah=std::make_shared<fsnet::EntityScriptHost>(std::make_shared<fsnet::NodeScriptHost>(a),database,"a/boot",runtime);
    auto bh=std::make_shared<fsnet::EntityScriptHost>(std::make_shared<fsnet::NodeScriptHost>(b),database,"b/boot",runtime);
    fsnet::LuaEngine ae(ah),be(bh);ae.load(model);be.load(model);
    fsnet::AttributeReplication ar(a,ah),br(b,bh);
    auto base=[](std::uint64_t,const std::string&)->std::optional<fsnet::CallIdentity>{return fsnet::CallIdentity{fsnet::Caller::server,"gateway"};};
    auto trust=[](const fsnet::CallIdentity& who,const std::string&){return who.caller==fsnet::Caller::server&&who.subject=="gateway";};
    fsnet::GatewayDelegation ad(a,base,trust,ah),bd(b,base,trust,bh);
    auto ap=a.listen("127.0.0.1",0),bp=b.listen("127.0.0.1",0);
    front.identify([](std::uint64_t,const std::string&)->std::optional<fsnet::CallIdentity>{return fsnet::CallIdentity{fsnet::Caller::client,"alice"};});
    fsnet::GatewayLimits limits;limits.refresh=2s;
    fsnet::ClientGateway gateway(front,back,database,{{"a","127.0.0.1",ap,{"Actor"}},{"b","127.0.0.1",bp,{"Actor"}}},limits);
    gateway.bind_account("login","Actor");gateway.bind_entity("Actor",{"move"});
    auto port=front.listen("127.0.0.1",0);auto peer=client.connect("127.0.0.1",port);
    std::uint64_t owned=0;fsnet::AttributeClient attributes(client,peer);
    auto pump=[&]{a.poll();b.poll();front.poll();back.poll();client.poll();ah->poll();bh->poll();ae.poll();be.poll();ad.poll();bd.poll();ar.poll();br.poll();gateway.poll();attributes.poll();};
    auto until=[&](auto done,std::source_location where=std::source_location::current()){auto end=std::chrono::steady_clock::now()+5s;while(!done()){pump();if(std::chrono::steady_clock::now()>=end){auto stats=gateway.stats();throw std::runtime_error("restore deadline at "+std::to_string(where.line())+" pending="+std::to_string(stats.pending)+" migrating="+std::to_string(stats.migrating)+" migrations="+std::to_string(stats.migrations)+" failures="+std::to_string(stats.failures)+": "+ae.last_error()+" / "+be.last_error());}std::this_thread::sleep_for(1ms);}};
    std::optional<fsnet::Result> result;ah->read_attributes("Actor","alice",[&](auto r){result=std::move(r);});until([&]{return result.has_value();});CHECK(result->status==fsnet::Status::ok);
    attributes.watch("Actor","alice",[&](auto status,auto id){CHECK(status==fsnet::Status::ok);owned=id;});until([&]{return owned!=0;});
    a.stop();
    until([&]{return gateway.stats().migrating==1;});
    auto before_expiry=std::chrono::steady_clock::now()+100ms;
    while(std::chrono::steady_clock::now()<before_expiry){pump();CHECK(!bh->active("Actor","alice"));std::this_thread::sleep_for(1ms);}
    // A stalled owner thread must not erase knowledge of the failed endpoint
    // just because its 250ms health cooldown elapsed.
    std::this_thread::sleep_for(600ms);
    until([&]{return gateway.stats().migrations==1&&!gateway.stats().migrating;});
    // Wait out the dead endpoint's health backoff within b's new 500ms lease,
    // while the old 2s lookup cache would still be fresh. A cache of a's expired
    // lease would select a again here.
    auto wait=std::chrono::steady_clock::now()+300ms;
    while(std::chrono::steady_clock::now()<wait){pump();std::this_thread::sleep_for(1ms);}
    result.reset();client.call(peer,"Actor.move",fsnet::entity_message("alice","1","after-restore"),3s,[&](auto r){result=std::move(r);});until([&]{return result.has_value();});
    std::cout<<"manual_migrations="<<gateway.stats().migrations<<" followup_status="<<int(result->status)<<"\n";
}
int main(){try{restored_route_expires_old_lookup();}catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}}

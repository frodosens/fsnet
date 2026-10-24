#include "fsnet/client_gateway.hpp"
#include "fsnet/entity_transfer.hpp"
#include "fsnet/attribute_replication.hpp"
#include "fsnet/lua_engine.hpp"
#include "fsnet/entity_message.hpp"
#include "fsnet/entity_record.hpp"
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
    std::uint64_t owned=0;fsnet::AttributeClient attributes(client,peer,{},[&](const fsnet::EntityPresence& event){if(event.kind==fsnet::EntityPresence::Kind::enter)owned=event.subscription;});
    auto pump=[&]{a.poll();b.poll();front.poll();back.poll();client.poll();ah->poll();bh->poll();ae.poll();be.poll();ad.poll();bd.poll();ar.poll();br.poll();gateway.poll();attributes.poll();};
    auto until=[&](auto done,std::source_location where=std::source_location::current()){auto end=std::chrono::steady_clock::now()+5s;while(!done()){pump();if(std::chrono::steady_clock::now()>=end){auto stats=gateway.stats();throw std::runtime_error("restore deadline at "+std::to_string(where.line())+" pending="+std::to_string(stats.pending)+" migrating="+std::to_string(stats.migrating)+" migrations="+std::to_string(stats.migrations)+" failures="+std::to_string(stats.failures)+": "+ae.last_error()+" / "+be.last_error());}std::this_thread::sleep_for(1ms);}};
    std::optional<fsnet::Result> result;ah->read_attributes("Actor","alice",[&](auto r){result=std::move(r);});until([&]{return result.has_value();});CHECK(result->status==fsnet::Status::ok);
    result.reset();client.call(peer,"login","",3s,[&](auto r){result=std::move(r);});until([&]{return result.has_value();});CHECK(result->status==fsnet::Status::ok);until([&]{return owned!=0;});
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
    CHECK(result->status==fsnet::Status::ok);
}
int main(){try{
    restored_route_expires_old_lookup();
    auto database=fsnet::memory_database();fsnet::Node a("internal"),b("internal"),front("clients"),back("internal"),alice("clients"),bob("clients"),intruder("internal");
    auto ah=std::make_shared<fsnet::EntityScriptHost>(std::make_shared<fsnet::NodeScriptHost>(a),database,"a/boot");
    auto bh=std::make_shared<fsnet::EntityScriptHost>(std::make_shared<fsnet::NodeScriptHost>(b),database,"b/boot");
    fsnet::LuaEngine ae(ah),be(bh);ae.load(model);be.load(model);
    fsnet::AttributeReplication ar(a,ah,[](const auto&,const auto&,const auto&){return true;}),br(b,bh,[](const auto&,const auto&,const auto&){return true;});
    bool enrolling=true;
    std::unordered_map<std::uint64_t,fsnet::CallIdentity> ai,bi;
    // Test harness enrolls physical connections only while it creates gateway
    // hops. Production replaces this fixture with verified certificate pins.
    a.authorize([&](std::uint64_t peer,const std::string&,const std::string&){if(enrolling&&!ai.contains(peer))ai.emplace(peer,fsnet::CallIdentity{fsnet::Caller::server,"gateway"});return true;});
    b.authorize([&](std::uint64_t peer,const std::string&,const std::string&){if(enrolling&&!bi.contains(peer))bi.emplace(peer,fsnet::CallIdentity{fsnet::Caller::server,"gateway"});return true;});
    auto resolver=[](auto& identities){return [&identities](std::uint64_t peer,const std::string&)->std::optional<fsnet::CallIdentity>{auto it=identities.find(peer);return it==identities.end()?fsnet::CallIdentity{fsnet::Caller::client,"intruder"}:it->second;};};
    auto trust=[](const fsnet::CallIdentity& who,const std::string&){return who.caller==fsnet::Caller::server&&who.subject=="gateway";};
    fsnet::GatewayDelegation ad(a,resolver(ai),trust,ah),bd(b,resolver(bi),trust,bh);
    auto ap=a.listen("127.0.0.1",0),bp=b.listen("127.0.0.1",0);std::uint64_t alice_front=0;std::string alice_subject="alice";
    front.identify([&](std::uint64_t peer,const std::string&){if(!alice_front)alice_front=peer;return std::optional(fsnet::CallIdentity{fsnet::Caller::client,peer==alice_front?alice_subject:"bob"});});
    fsnet::GatewayLimits limits;limits.refresh=5ms;limits.migration_timeout=3s;
    fsnet::ClientGateway gateway(front,back,database,{{"a","127.0.0.1",ap,{"Actor","Pet"}},{"b","127.0.0.1",bp,{"Actor","Pet"}}},limits);
    std::vector<fsnet::EntityEndpoint> owners={{"a","127.0.0.1",ap,{"Actor","Pet"}},{"b","127.0.0.1",bp,{"Actor","Pet"}}};
    auto owner_trust=[](const fsnet::CallIdentity& who){return who.caller==fsnet::Caller::server&&who.subject=="gateway";};
    fsnet::EntityTransfer at(a,ah,database,owners,owner_trust),bt(b,bh,database,owners,owner_trust);
    gateway.bind_entity("Actor",{"move","server_only"});gateway.bind_account("login","Actor");gateway.bind_account("pet_login","Pet");
    auto port=front.listen("127.0.0.1",0);auto alice_peer=alice.connect("127.0.0.1",port);
    std::uint64_t owned=0,pet=0;bool actor_controlled=false,pet_controlled=false;std::size_t leaves=0;
    fsnet::AttributeClient ac(alice,alice_peer,{},[&](const fsnet::EntityPresence& event){
        if(event.kind==fsnet::EntityPresence::Kind::enter){CHECK(event.owned);if(event.type=="Actor")owned=event.subscription;else pet=event.subscription;}
        if(event.type=="Actor")actor_controlled=event.controlled;else pet_controlled=event.controlled;
        if(event.kind==fsnet::EntityPresence::Kind::leave)++leaves;
    });
    std::unique_ptr<fsnet::AttributeClient> bc;
    auto pump=[&]{a.poll();b.poll();front.poll();back.poll();alice.poll();bob.poll();intruder.poll();ah->poll();bh->poll();ae.poll();be.poll();ad.poll();bd.poll();ar.poll();br.poll();gateway.poll();at.poll();bt.poll();ac.poll();if(bc)bc->poll();};
    auto until=[&](auto condition,std::source_location where=std::source_location::current()){auto end=std::chrono::steady_clock::now()+8s;while(!condition()){pump();if(std::chrono::steady_clock::now()>end)throw std::runtime_error("deadline at "+std::to_string(where.line())+": "+ae.last_error()+" / "+be.last_error());std::this_thread::sleep_for(1ms);}};
    auto call=[&](fsnet::Node& node,std::uint64_t peer,std::string route,std::string data){std::optional<fsnet::Result> result;node.call(peer,std::move(route),std::move(data),3s,[&](auto r){result=std::move(r);});until([&]{return result.has_value();});return *result;};
    std::optional<fsnet::Result> loaded;ah->read_attributes("Actor","alice",[&](auto r){loaded=std::move(r);});until([&]{return loaded.has_value();});CHECK(loaded->status==fsnet::Status::ok);loaded.reset();
    bh->read_attributes("Actor","bob",[&](auto r){loaded=std::move(r);});until([&]{return loaded.has_value();});CHECK(loaded->status==fsnet::Status::ok);
    CHECK(call(alice,alice_peer,"login","bob").data=="alice");until([&]{return owned&&ac.get(owned);});auto original=owned;
    auto x=[](const fsnet::AttributeReplica* replica){return std::get<double>(std::get<fsnet::AttributeStruct>(replica->values().at("position")).at("x"));};
    CHECK(ac.get(owned)->values().contains("gold"));
    auto bob_peer=bob.connect("127.0.0.1",port);bc=std::make_unique<fsnet::AttributeClient>(bob,bob_peer);
    std::uint64_t observer=0,other=0;
    bc->watch("Actor","alice",[&](auto status,auto id){CHECK(status==fsnet::Status::ok);observer=id;});until([&]{return observer!=0;});CHECK(bc->get(observer)->values().size()==1);
    ac.watch("Actor","bob",[&](auto status,auto id){CHECK(status==fsnet::Status::ok);other=id;});until([&]{return other!=0;});CHECK(other!=owned&&ac.get(other)->values().size()==1);
    auto move=[&](std::string delta){return call(alice,alice_peer,"Actor.move",fsnet::entity_message("alice",delta));};
    CHECK(move("5").status==fsnet::Status::ok);until([&]{return x(ac.get(owned))==5&&x(bc->get(observer))==5;});
    CHECK(call(alice,alice_peer,"Actor.server_only",fsnet::entity_message("alice","")).status==fsnet::Status::rejected);
    CHECK(call(bob,bob_peer,"Actor.move",fsnet::entity_message("alice","100")).status==fsnet::Status::rejected);
    auto write=[&](fsnet::AttributeClient& client,std::uint64_t id,double value){std::optional<fsnet::Status> result;client.write(id,{{"position",fsnet::AttributeStruct{{"x",value},{"y",0.0},{"z",0.0}}}},[&](auto s){result=s;});until([&]{return result.has_value();});return *result;};
    CHECK(write(ac,owned,7)==fsnet::Status::ok);until([&]{return x(ac.get(owned))==7&&x(bc->get(observer))==7;});
    CHECK(write(ac,owned,99)==fsnet::Status::rejected);CHECK(write(*bc,observer,8)==fsnet::Status::rejected);CHECK(x(ac.get(owned))==7);
    std::string stolen;for(int i=7;i>=0;--i)stolen+=static_cast<char>(owned>>(i*8));
    CHECK(call(bob,bob_peer,"system.attributes.renew",stolen).status==fsnet::Status::rejected);
    loaded.reset();bh->read_attributes("Pet","alice",[&](auto r){loaded=std::move(r);});until([&]{return loaded.has_value();});CHECK(loaded->status==fsnet::Status::ok);
    CHECK(call(alice,alice_peer,"pet_login","").status==fsnet::Status::ok);until([&]{return pet&&pet_controlled&&!actor_controlled;});
    enrolling=false;auto direct=intruder.connect("127.0.0.1",ap);CHECK(call(intruder,direct,"system.gateway.delegate","alice").status==fsnet::Status::rejected);
    // Backend subscription IDs start at one on each owner; gateway IDs stay unique.
    CHECK(gateway.stats().subscriptions==4);
    enrolling=true;auto handle=ah->handle("Actor","alice");CHECK(handle);std::optional<fsnet::Result> released;at.transfer("Actor","alice","b",[&](auto r){released=std::move(r);});until([&]{return released.has_value();});CHECK(released->status==fsnet::Status::ok);
    until([&]{return gateway.stats().migrations>=2&&!gateway.stats().migrating&&bh->active("Actor","alice");});
    CHECK(owned==original&&ac.get(owned)&&bc->get(observer)&&leaves==0);CHECK(x(ac.get(owned))==7);
    CHECK(std::get<std::int64_t>(ac.get(owned)->values().at("activations"))==2);
    CHECK(move("2").status==fsnet::Status::ok);until([&]{return x(ac.get(owned))==9&&x(bc->get(observer))==9;});
    // Return to a specified owner without disturbing the controlled Pet on b.
    released.reset(); auto migrations=gateway.stats().migrations;
    bt.transfer("Actor","alice","a",[&](auto r){released=std::move(r);});
    until([&]{return released.has_value();});CHECK(released->status==fsnet::Status::ok);
    until([&]{return gateway.stats().migrations>=migrations+2&&!gateway.stats().migrating&&ah->active("Actor","alice");});
    CHECK(owned==original&&leaves==0&&x(ac.get(owned))==9&&pet_controlled&&!actor_controlled);
    // The currently possessed entity migrates too: stable ID and restored control.
    auto original_pet=pet; migrations=gateway.stats().migrations; released.reset();
    bt.transfer("Pet","alice","a",[&](auto r){released=std::move(r);});
    until([&]{return released.has_value();});CHECK(released->status==fsnet::Status::ok);
    until([&]{return gateway.stats().migrations>=migrations+1&&!gateway.stats().migrating&&pet_controlled;});
    CHECK(pet==original_pet&&leaves==0&&!actor_controlled&&ah->active("Pet","alice")&&!bh->active("Pet","alice"));
    std::optional<fsnet::Result> destroyed;ah->control_entity("Actor","alice",true,[&](auto r){destroyed=std::move(r);});until([&]{return destroyed.has_value();});CHECK(destroyed->status==fsnet::Status::ok);
    until([&]{return !ac.get(owned)&&!bc->get(observer);});CHECK(!database->get(fsnet::entity_record_key("Actor","alice")));
    for(int i=0;i<20;++i)pump();CHECK(!ah->handle("Actor","alice"));
    alice_subject="mallory";until([&]{return !ac.get(other);});CHECK(gateway.stats().clients==1);
    front.set_ready(false);CHECK(call(bob,bob_peer,"system.health","").status==fsnet::Status::overloaded);front.set_ready(true);
    CHECK(call(bob,bob_peer,"system.health","").data=="ready");
    gateway.drain();CHECK(call(bob,bob_peer,"Actor.move",fsnet::entity_message("bob","1")).status==fsnet::Status::overloaded);
    at.stop();bt.stop();gateway.stop();CHECK(back.pending()==0&&back.stats().connections==0);ac.stop();bc->stop();ae.stop();be.stop();ar.stop();br.stop();ad.stop();bd.stop();ah->stop();bh->stop();
    CHECK(gateway.stats().pending==0&&gateway.stats().subscriptions==0);
    std::cout<<"PASS gateway: trusted client delegation, role/owner isolation, shared patches, ID remap, automatic/manual migration, deletion and identity revocation\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}

#include <condition_variable>
#include <fstream>
#include <mutex>
#include <source_location>
#include "fsnet/entity_transfer.hpp"
#include "fsnet/entity_message.hpp"
#include "fsnet/lua_engine.hpp"
#include "fsnet/client_gateway.hpp"
#include "fsnet/attribute_replication.hpp"
#include "fsnet/entity_record.hpp"
#include "fsnet/replication_wire.hpp"
#include <iostream>
#include <thread>
using namespace std::chrono_literals;
#define CHECK(x) do { if(!(x)) throw std::runtime_error(#x); } while(false)
void control_confirmation_race(){
 auto db=fsnet::memory_database();
 for(auto type:{"Actor","Pet"}) { fsnet::EntityRecord rec;rec.owner="a/boot";rec.expires_ms=INT64_MAX;db->compare_exchange(fsnet::entity_record_key(type,"alice"),0,fsnet::encode_entity_record(rec)); }
 fsnet::Node owner("internal"),front("clients"),back("internal"),client("clients");
 front.identify([](auto,const auto&)->std::optional<fsnet::CallIdentity>{return fsnet::CallIdentity{fsnet::Caller::client,"alice"};});
 fsnet::AttributeUpdate image{0,1,true,{{"x",std::int32_t(7)}}};
 auto presence=[&](int kind,int flags,std::uint64_t id,std::string type){return std::string{char(kind),char(flags)}+fsnet::encode_replication_packet(id,type,"alice",fsnet::Status::ok,&image,30000);};
 std::uint64_t initial_peer=0,new_peer=0;fsnet::Reply actor_restore;bool actor_intent=false; unsigned reconciled=0;
 owner.bind("system.gateway.delegate",[](auto,auto reply){reply({fsnet::Status::ok,{}});});
 owner.bind_rpc("login",{true,false,false},[&](const auto& who,auto,auto reply){initial_peer=who.peer;owner.notify(who.peer,"system.entities.presence",presence(1,3,11,"Actor"));owner.notify(who.peer,"system.entities.presence",presence(1,1,12,"Pet"));reply({fsnet::Status::ok,{}});});
 owner.bind_rpc("system.gateway.restore",{true,false,false},[&](const auto& who,std::string data,auto reply){std::string_view in(data);auto [type,id]=fsnet::decode_entity_key(in);new_peer=who.peer;if(type=="Actor"){if(in.front()){actor_intent=true;actor_restore=std::move(reply);}else{++reconciled;reply({fsnet::Status::ok,presence(1,1,21,"Actor")});}}else reply({fsnet::Status::ok,presence(1,1,22,"Pet")});});
 owner.bind("system.attributes.unwatch",[](auto,auto reply){reply({fsnet::Status::ok,{}});});
 owner.bind("system.gateway.unpossess",[](auto,auto reply){reply({fsnet::Status::ok,{}});});
 auto port=owner.listen("127.0.0.1",0);
 fsnet::GatewayLimits limits;limits.refresh=5ms;limits.request_timeout=500ms;limits.migration_timeout=3s;
 fsnet::ClientGateway gateway(front,back,db,{{"a","127.0.0.1",port,{"Actor","Pet"}}},limits);gateway.bind_legacy_account("login","Actor");
 auto peer=client.connect("127.0.0.1",front.listen("127.0.0.1",0));std::uint64_t actor=0,pet=0;unsigned actor_leaves=0;bool pet_controlled=false;
 fsnet::AttributeClient attributes(client,peer,{},[&](const fsnet::EntityPresence& p){if(p.type=="Actor"){if(p.kind==fsnet::EntityPresence::Kind::enter)actor=p.subscription;if(p.kind==fsnet::EntityPresence::Kind::leave)++actor_leaves;}else {pet=p.subscription;pet_controlled=p.controlled;}});
 auto pump=[&]{owner.poll();front.poll();back.poll();client.poll();gateway.poll();attributes.poll();};
 auto until=[&](auto done){auto deadline=std::chrono::steady_clock::now()+3s;while(!done()){pump();CHECK(std::chrono::steady_clock::now()<deadline);std::this_thread::sleep_for(1ms);}};
 bool login=false;client.call(peer,"login","",1s,[&](auto r){CHECK(r.status==fsnet::Status::ok);login=true;});until([&]{return login&&actor&&pet;});
 owner.close(initial_peer);until([&]{return actor_restore&&gateway.stats().migrations==1;});CHECK(actor_intent);
 // Another entity becomes controlled while Actor's restore is still pending.
 owner.notify(new_peer,"system.entities.presence",presence(3,3,22,"Pet"));until([&]{return pet_controlled;});
 bool actor_before=attributes.get(actor)!=nullptr;
 actor_restore({fsnet::Status::ok,presence(1,3,21,"Actor")});until([&]{return gateway.stats().migrations==2||actor_leaves>0;});
 CHECK(reconciled==1&&actor_before&&attributes.get(actor)&&actor_leaves==0&&pet_controlled&&gateway.stats().subscriptions==2);
 std::cout<<"restore_sent_control="<<actor_intent<<" pet_controlled="<<pet_controlled<<" actor_before_confirmation="<<actor_before<<" actor_after_confirmation="<<bool(attributes.get(actor))<<" actor_leaves="<<actor_leaves<<" gateway_subscriptions="<<gateway.stats().subscriptions<<"\n";
}
const char* migration_model=R"(
    local Actor=fsnet.Entity:extend('Actor',{owner='legacy_id'})
    Actor:attr('position',{type='vec3',client='rw',sync='observers'})
    Actor:attr('gold',{type='int32',default=0,sync='owner'})
    Actor:attr('activations',{type='int32',default=0,sync='owner'})
    function Actor:on_activate() self.activations=self.activations+1; if self.activations>1 then runtime.sleep(250) end end
    Actor:rpc('move',{callers={'client'},access='owner'})
    function Actor:move(data) self.position.x=self.position.x+assert(tonumber(data)); self.gold=self.gold+1; return self.id end
    Actor:rpc('server_only',{callers={'server'}})
    function Actor:server_only() return 'secret' end
    function Actor:validate_write(values) return not values.position or values.position.x<=20 end
    Actor:register()
    local Pet=Actor:extend('Pet'); Pet:register()
    runtime.legacy_rpc('pet_login',{callers={'client'},handler=function(ctx) local actor=Pet:legacy_spawn_or_restore(ctx.subject); actor:show_to(ctx.peer); actor:possess(ctx.peer); return actor.id end})
    runtime.legacy_rpc('login',{callers={'client'},handler=function(ctx,name)
        local actor=Actor:legacy_spawn_or_restore(ctx.subject)
        actor:show_to(ctx.peer); actor:possess(ctx.peer); return actor.id
    end})
)";

void control_during_handoff(){
 auto db=fsnet::memory_database();fsnet::Node a("internal"),b("internal"),front("clients"),back("internal"),client("clients");
 auto ah=std::make_shared<fsnet::EntityScriptHost>(std::make_shared<fsnet::NodeScriptHost>(a),db,"a/boot");
 auto bh=std::make_shared<fsnet::EntityScriptHost>(std::make_shared<fsnet::NodeScriptHost>(b),db,"b/boot");
 fsnet::LuaEngine ae(ah),be(bh);ae.load(migration_model);be.load(migration_model);
 fsnet::AttributeReplication ar(a,ah),br(b,bh);
 auto base=[](std::uint64_t,const std::string&)->std::optional<fsnet::CallIdentity>{return fsnet::CallIdentity{fsnet::Caller::server,"gateway"};};
 auto trust=[](const fsnet::CallIdentity& who,const std::string&){return who.caller==fsnet::Caller::server&&who.subject=="gateway";};
 fsnet::GatewayDelegation ad(a,base,trust,ah),bd(b,base,trust,bh);
 auto ap=a.listen("127.0.0.1",0),bp=b.listen("127.0.0.1",0);
 front.identify([](std::uint64_t,const std::string&)->std::optional<fsnet::CallIdentity>{return fsnet::CallIdentity{fsnet::Caller::client,"alice"};});
 fsnet::GatewayLimits limits;limits.refresh=5ms;
 std::vector<fsnet::EntityEndpoint> endpoints={{"a","127.0.0.1",ap,{"Actor","Pet"}},{"b","127.0.0.1",bp,{"Actor","Pet"}}};
 fsnet::ClientGateway gateway(front,back,db,endpoints,limits);gateway.bind_legacy_account("login","Actor");gateway.bind_legacy_account("pet_login","Pet");
 auto transfer_trust=[](const auto& who){return who.caller==fsnet::Caller::server&&who.subject=="gateway";};
 fsnet::EntityTransfer at(a,ah,db,endpoints,transfer_trust),bt(b,bh,db,endpoints,transfer_trust);
 auto peer=client.connect("127.0.0.1",front.listen("127.0.0.1",0));std::uint64_t actor=0,pet=0;bool actor_control=false;
 fsnet::AttributeClient attributes(client,peer,{},[&](const fsnet::EntityPresence& p){if(p.type=="Actor"){actor=p.subscription;actor_control=p.controlled;}else pet=p.subscription;});
 auto pump=[&]{a.poll();b.poll();front.poll();back.poll();client.poll();ah->poll();bh->poll();ae.poll();be.poll();ad.poll();bd.poll();ar.poll();br.poll();gateway.poll();at.poll();bt.poll();attributes.poll();};
 auto until=[&](auto done){auto end=std::chrono::steady_clock::now()+4s;while(!done()){pump();CHECK(std::chrono::steady_clock::now()<end);std::this_thread::sleep_for(1ms);}};
 std::optional<fsnet::Result> r;ah->read_attributes("Actor","alice",[&](auto x){r=std::move(x);});until([&]{return r.has_value();});CHECK(r->status==fsnet::Status::ok);
 r.reset();bh->read_attributes("Pet","alice",[&](auto x){r=std::move(x);});until([&]{return r.has_value();});CHECK(r->status==fsnet::Status::ok);
 auto call=[&](std::string route){r.reset();client.call(peer,route,"",2s,[&](auto x){r=std::move(x);});until([&]{return r.has_value();});return r->status;};
 CHECK(call("pet_login")==fsnet::Status::ok);until([&]{return pet;});CHECK(call("login")==fsnet::Status::ok);until([&]{return actor&&actor_control;});
 std::optional<fsnet::Result> transferred;at.transfer("Actor","alice","b",[&](auto x){transferred=std::move(x);});
 until([&]{return gateway.stats().migrating>0;});bool connected_before=client.connected(peer);auto pet_status=call("pet_login");
 until([&]{return !client.connected(peer)||transferred.has_value();});
 for(int i=0;i<50;++i){pump();std::this_thread::sleep_for(1ms);}
 std::cout<<"connected_before_possess="<<connected_before<<" pet_login_status="<<int(pet_status)<<" connected_after_possess="<<client.connected(peer)<<" actor_active_on_target="<<bh->active("Actor","alice")<<"\n";
 CHECK(connected_before&&pet_status==fsnet::Status::ok&&client.connected(peer)&&bh->active("Actor","alice"));
}
const char* manual_model=R"(
    local Actor=fsnet.Entity:extend('Actor',{owner='legacy_id'})
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
    runtime.legacy_rpc('pet_login',{callers={'client'},handler=function(ctx) local actor=Pet:legacy_spawn_or_restore(ctx.subject); actor:show_to(ctx.peer); actor:possess(ctx.peer); return actor.id end})
    runtime.legacy_rpc('login',{callers={'client'},handler=function(ctx,name)
        local actor=Actor:legacy_spawn_or_restore(ctx.subject)
        actor:show_to(ctx.peer); actor:possess(ctx.peer); return actor.id
    end})
)";
void restored_route_expires_old_lookup(){
    auto database=fsnet::memory_database();fsnet::Node a("internal"),b("internal"),front("clients"),back("internal"),client("clients");
    fsnet::EntityRuntimeLimits runtime;runtime.lease=500ms;runtime.request_timeout=50ms;
    auto ah=std::make_shared<fsnet::EntityScriptHost>(std::make_shared<fsnet::NodeScriptHost>(a),database,"a/boot",runtime);
    auto bh=std::make_shared<fsnet::EntityScriptHost>(std::make_shared<fsnet::NodeScriptHost>(b),database,"b/boot",runtime);
    fsnet::LuaEngine ae(ah),be(bh);ae.load(manual_model);be.load(manual_model);
    fsnet::AttributeReplication ar(a,ah),br(b,bh);
    auto base=[](std::uint64_t,const std::string&)->std::optional<fsnet::CallIdentity>{return fsnet::CallIdentity{fsnet::Caller::server,"gateway"};};
    auto trust=[](const fsnet::CallIdentity& who,const std::string&){return who.caller==fsnet::Caller::server&&who.subject=="gateway";};
    fsnet::GatewayDelegation ad(a,base,trust,ah),bd(b,base,trust,bh);
    auto ap=a.listen("127.0.0.1",0),bp=b.listen("127.0.0.1",0);
    front.identify([](std::uint64_t,const std::string&)->std::optional<fsnet::CallIdentity>{return fsnet::CallIdentity{fsnet::Caller::client,"alice"};});
    fsnet::GatewayLimits limits;limits.refresh=2s;
    fsnet::ClientGateway gateway(front,back,database,{{"a","127.0.0.1",ap,{"Actor"}},{"b","127.0.0.1",bp,{"Actor"}}},limits);
    gateway.bind_legacy_account("login","Actor");gateway.bind_legacy_entity("Actor",{"move"});
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
    CHECK(result->status==fsnet::Status::ok);
    std::cout<<"manual_migrations="<<gateway.stats().migrations<<" followup_status="<<int(result->status)<<"\n";
}
using Clock=std::chrono::steady_clock;
struct BlockingDb:fsnet::Storage {
 std::mutex mutex;std::condition_variable cv;bool blocked=true;std::size_t reads=0;
 std::optional<fsnet::Record> get(std::string_view) override {std::unique_lock lock(mutex);++reads;cv.wait(lock,[&]{return !blocked;});return {};}
 bool compare_exchange(std::string_view,std::uint64_t,std::string_view)override{return false;}
 bool erase(std::string_view,std::uint64_t)override{return false;}
 void unblock(){std::lock_guard lock(mutex);blocked=false;cv.notify_all();}
};
std::size_t rss(){std::ifstream f("/proc/self/status");std::string key;while(f>>key){if(key=="VmRSS:"){std::size_t value;f>>value;return value;}std::string line;std::getline(f,line);}return 0;}
void timeout_detaches_waiters(){
 auto db=std::make_shared<BlockingDb>();
 fsnet::Node front("x"),back("y"),client("x");
 front.identify([](auto,const auto&){return std::optional(fsnet::CallIdentity{fsnet::Caller::client,"alice"});});
 fsnet::GatewayLimits limits;limits.requests=4;limits.bytes=1024*1024;limits.request_timeout=50ms;
 fsnet::ClientGateway gateway(front,back,db,{{"owner","127.0.0.1",1,{"Actor"}}},limits);gateway.bind_legacy_entity("Actor",{"echo"});
 struct Unblock { std::shared_ptr<BlockingDb> db; ~Unblock(){db->unblock();} } release{db};
 auto peer=client.connect("127.0.0.1",front.listen("127.0.0.1",0));
 auto pump=[&]{front.poll();back.poll();client.poll();gateway.poll();};
 auto initial=Clock::now()+100ms;while(Clock::now()<initial){pump();std::this_thread::sleep_for(1ms);}
 auto baseline=rss();std::size_t completed=0,timeouts=0;auto data=fsnet::entity_message("alice",std::string(240*1024,'x'));
 for(unsigned round=1;round<=30;++round){
  for(unsigned i=0;i<4;++i)client.call(peer,"Actor.echo",data,1s,[&](auto result){++completed;if(result.status==fsnet::Status::timeout)++timeouts;});
  auto end=Clock::now()+2s;while(completed<round*4&&Clock::now()<end){pump();std::this_thread::sleep_for(1ms);}
  auto s=gateway.stats();CHECK(s.pending==0&&s.queued_bytes==0&&s.lookup_waiters==0&&s.link_waiters==0);if(round==1||round==10||round==20||round==30)std::cout<<"{\"round\":"<<round<<",\"completed\":"<<completed<<",\"timeouts\":"<<timeouts<<",\"pending\":"<<s.pending<<",\"queued_bytes\":"<<s.queued_bytes<<",\"rss_kib\":"<<rss()<<",\"rss_delta_kib\":"<<rss()-baseline<<"}"<<std::endl;
 }
 CHECK(completed==120&&timeouts==120);
 db->unblock();for(unsigned i=0;i<100;++i){pump();std::this_thread::sleep_for(1ms);}gateway.stop();
}

void coalesced_request_order(){
 auto db=std::make_shared<BlockingDb>();fsnet::Node owner("internal"),front("clients"),back("internal"),client("clients");
 front.identify([](auto,const auto&){return std::optional(fsnet::CallIdentity{fsnet::Caller::client,"alice"});});
 fsnet::Reply delegated;std::vector<std::string> order;
 owner.bind("system.gateway.delegate",[&](auto,auto reply){delegated=std::move(reply);});
 owner.bind("Actor.echo",[&](std::string input,auto reply){auto entity=fsnet::parse_entity_message(input);CHECK(entity);order.push_back(entity->payload);reply({fsnet::Status::ok,{}});});
 auto port=owner.listen("127.0.0.1",0);fsnet::ClientGateway gateway(front,back,db,{{"owner","127.0.0.1",port,{"Actor"}}});gateway.bind_legacy_entity("Actor",{"echo"});
 struct Unblock {std::shared_ptr<BlockingDb> db;~Unblock(){db->unblock();}} release{db};
 auto peer=client.connect("127.0.0.1",front.listen("127.0.0.1",0));unsigned completed=0;
 auto pump=[&]{owner.poll();front.poll();back.poll();client.poll();gateway.poll();};
 auto until=[&](auto done){auto end=Clock::now()+3s;while(!done()){pump();CHECK(Clock::now()<end);std::this_thread::sleep_for(1ms);}};
 for(int i=0;i<4;++i)client.call(peer,"Actor.echo",fsnet::entity_message("alice",std::to_string(i)),3s,[&](auto result){CHECK(result.status==fsnet::Status::ok);++completed;});
 until([&]{return gateway.stats().lookup_waiters==4;});db->unblock();
 until([&]{return bool(delegated)&&gateway.stats().link_waiters==4;});delegated({fsnet::Status::ok,{}});
 until([&]{return completed==4;});CHECK((order==std::vector<std::string>{"0","1","2","3"}));
 CHECK(gateway.stats().lookup_waiters==0&&gateway.stats().link_waiters==0);
}
int main(){try{control_confirmation_race();control_during_handoff();restored_route_expires_old_lookup();timeout_detaches_waiters();coalesced_request_order();}catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}}

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
    function Actor:on_activate() self.activations=self.activations+1; if self.activations>1 then runtime.sleep(250) end end
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

int main(){try{
 auto db=fsnet::memory_database();fsnet::Node a("internal"),b("internal"),front("clients"),back("internal"),client("clients");
 auto ah=std::make_shared<fsnet::EntityScriptHost>(std::make_shared<fsnet::NodeScriptHost>(a),db,"a/boot");
 auto bh=std::make_shared<fsnet::EntityScriptHost>(std::make_shared<fsnet::NodeScriptHost>(b),db,"b/boot");
 fsnet::LuaEngine ae(ah),be(bh);ae.load(model);be.load(model);
 fsnet::AttributeReplication ar(a,ah),br(b,bh);
 auto base=[](std::uint64_t,const std::string&)->std::optional<fsnet::CallIdentity>{return fsnet::CallIdentity{fsnet::Caller::server,"gateway"};};
 auto trust=[](const fsnet::CallIdentity& who,const std::string&){return who.caller==fsnet::Caller::server&&who.subject=="gateway";};
 fsnet::GatewayDelegation ad(a,base,trust,ah),bd(b,base,trust,bh);
 auto ap=a.listen("127.0.0.1",0),bp=b.listen("127.0.0.1",0);
 front.identify([](std::uint64_t,const std::string&)->std::optional<fsnet::CallIdentity>{return fsnet::CallIdentity{fsnet::Caller::client,"alice"};});
 fsnet::GatewayLimits limits;limits.refresh=5ms;
 std::vector<fsnet::EntityEndpoint> endpoints={{"a","127.0.0.1",ap,{"Actor","Pet"}},{"b","127.0.0.1",bp,{"Actor","Pet"}}};
 fsnet::ClientGateway gateway(front,back,db,endpoints,limits);gateway.bind_account("login","Actor");gateway.bind_account("pet_login","Pet");
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
 return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}

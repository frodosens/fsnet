#include "fsnet/lua_engine.hpp"
#include "fsnet/entity_host.hpp"
#include "fsnet/client_script_host.hpp"
#include <chrono>
#include <iostream>
#include <thread>
#include <unordered_map>
using namespace std::chrono_literals;
#define CHECK(x) do { if(!(x)) throw std::runtime_error(std::string(__FILE__)+':'+std::to_string(__LINE__)+" " #x); } while(false)
const char* shared=R"(
    local Player=fsnet.Entity:extend('Player',{owner='legacy_id'})
    Player:attr('name',{type='string',default='',sync='observers'})
    Player:attr('x',{type='int32',default=0,authority='driver',sync='observers'})
    Player:attr('gold',{type='int32',default=7,sync='owner'})
    Player:attr('session',{type='int32',default=0,persist=false,sync='owner'})
    Player:rpc('step',{callers={'client'},access='owner'})
    function Player:step() self.x=self.x+1 end
    Player:define()
    local Drone=Player:extend('Drone')
    Drone:define()
)";
const char* client_script=R"(
    local log={spawn=0,remote=0,possess=0,unpossess=0,despawn=0}
    local camera
    local fail_factory=false
    local LocalPlayer={}
    function LocalPlayer:on_spawn()
        assert(self.owned and self.entity.name==account and self.entity.gold==7)
        log.spawn=log.spawn+1
    end
    function LocalPlayer:on_possess()
        assert(not camera,'two controlled entities')
        self.prediction=self.entity:prediction()
        camera=self.type..'/'..self.id
        log.possess=log.possess+1
    end
    function LocalPlayer:on_unpossess()
        assert(camera==self.type..'/'..self.id)
        self.prediction:close(); camera=nil; log.unpossess=log.unpossess+1
    end
    function LocalPlayer:on_despawn() assert(not self.controlled); log.despawn=log.despawn+1 end
    local RemotePlayer={}
    function RemotePlayer:on_spawn()
        assert(not self.owned and self.entity.name==self.id)
        assert(not pcall(function() return self.entity.gold end))
        log.remote=log.remote+1
        if fail_factory then error('factory failure') end
    end
    function RemotePlayer:on_possess() error('remote player cannot be possessed') end
    function RemotePlayer:on_despawn() log.despawn=log.despawn+1 end
    fsnet.entities:register(Player,{owned=LocalPlayer,observed=RemotePlayer})
    fsnet.entities:register(Drone,{owned=LocalPlayer,observed=RemotePlayer})
    fsnet.entities:start()
    runtime.start(function()
        assert(runtime.request(peer,'login','forged-account')==account)
    end)
    runtime.legacy_rpc('arm',function() fail_factory=true; return '' end)
    runtime.legacy_rpc('inspect',function()
        local other=fsnet.entities:get('Player',account=='alice' and 'bob' or 'alice')
        local own=fsnet.entities:get('Player',account)
        if own then assert(own.entity.name==account and own.entity.session==9) end
        return table.concat({camera or '-',log.spawn,log.remote,log.possess,log.unpossess,log.despawn,
            own and own.entity.x or -1,other and other.entity.x or -1},':')
    end)
    runtime.legacy_rpc('command',function(payload)
        if payload=='unauthorized' then return pcall(Player.watch,Player,'bob') and 'allowed' or 'denied' end
        if payload=='step' then return Player:legacy_ref(account,{peer=peer}):step('') end
        return runtime.request(peer,payload,'')
    end)
)";
int main() {
    try {
        fsnet::Node server("presence"),alice("presence"),bob("presence"),intruder("presence");
        std::unordered_map<std::uint64_t,std::string> accounts;
        std::vector<std::uint64_t> peers;
        server.identify([&](std::uint64_t peer,const std::string&) {
            if(!accounts.contains(peer)) { accounts[peer]=peers.empty()?"alice":"bob"; peers.push_back(peer); }
            return std::optional(fsnet::CallIdentity{fsnet::Caller::client,accounts.at(peer)});
        });
        auto transport=std::make_shared<fsnet::NodeScriptHost>(server);
        auto entities=std::make_shared<fsnet::EntityScriptHost>(transport,fsnet::memory_database(),"presence/boot");
        fsnet::ReplicationLimits limits; limits.per_peer=3;
        fsnet::AttributeReplication replication(server,entities,{},limits);
        fsnet::LuaEngine authority(entities);
        authority.load(std::string(shared)+R"(
            Player:register(); Drone:register()
            local sessions={}
            runtime.legacy_rpc('login',{callers={'client'},handler=function(ctx,payload)
                assert(ctx.subject~='' and not sessions[ctx.subject])
                local player=Player:legacy_spawn_or_restore(ctx.subject,{name=ctx.subject,session=9})
                player:show_to(ctx.peer)
                player:show_to(ctx.peer) -- idempotent, no duplicate client factory
                for subject,peer in pairs(sessions) do
                    player:show_to(peer)
                    Player:legacy_ref(subject):show_to(ctx.peer)
                end
                player:possess(ctx.peer)
                sessions[ctx.subject]=ctx.peer
                return ctx.subject
            end})
            runtime.legacy_rpc('restore',{callers={'client'},handler=function(ctx)
                Player:legacy_spawn_or_restore(ctx.subject,{name='must-not-overwrite',x=999,session=1})
                return ''
            end})
            runtime.legacy_rpc('switch',{callers={'client'},handler=function(ctx)
                local drone=Drone:legacy_spawn_or_restore(ctx.subject,{name=ctx.subject})
                drone:show_to(ctx.peer); drone:possess(ctx.peer); drone:possess(ctx.peer)
                return ''
            end})
            runtime.legacy_rpc('limits',{callers={'client'},handler=function(ctx)
                assert(not pcall(function() Player:legacy_ref('missing'):show_to(ctx.peer) end))
                local extra=Player:legacy_spawn_or_restore('extra',{name='extra'})
                assert(not pcall(function() extra:show_to(ctx.peer) end))
                return ''
            end})
            runtime.legacy_rpc('fault',{callers={'client'},handler=function(ctx)
                Player:legacy_ref('alice'):hide_from(ctx.peer)
                Player:legacy_ref('alice'):show_to(ctx.peer)
                return ''
            end})
            runtime.legacy_rpc('back',{callers={'client'},handler=function(ctx)
                Player:legacy_ref(ctx.subject):possess(ctx.peer)
                Drone:legacy_ref(ctx.subject):hide_from(ctx.peer)
                return ''
            end})
            runtime.legacy_rpc('hide',{callers={'client'},handler=function(ctx)
                Player:legacy_ref('bob'):hide_from(ctx.peer); Player:legacy_ref('bob'):hide_from(ctx.peer)
                return ''
            end})
            runtime.legacy_rpc('show',{callers={'client'},handler=function(ctx)
                Player:legacy_ref('bob'):show_to(ctx.peer)
                assert(not pcall(function() Player:legacy_ref('bob'):possess(ctx.peer) end))
                return ''
            end})
        )");
        auto port=server.listen("127.0.0.1",0);
        auto a=alice.connect("127.0.0.1",port);
        auto ah=std::make_shared<fsnet::ClientScriptHost>(alice,a);
        fsnet::LuaEngine ae(ah);
        ae.load("local account='alice'; local peer="+std::to_string(a)+"\n"+shared+client_script);
        std::shared_ptr<fsnet::ClientScriptHost> bh;
        std::unique_ptr<fsnet::LuaEngine> be;
        auto pump=[&] { server.poll(); alice.poll(); bob.poll(); intruder.poll(); entities->poll(); replication.poll(); ah->poll(); authority.poll(); ae.poll(); if(bh) bh->poll(); if(be) be->poll(); };
        auto until=[&](auto done) {
            auto deadline=std::chrono::steady_clock::now()+5s;
            while(!done()) { pump(); if(std::chrono::steady_clock::now()>deadline) throw std::runtime_error("deadline: "+authority.last_error()+" / "+ae.last_error()+" / "+(be?be->last_error():"")); std::this_thread::sleep_for(1ms); }
        };
        auto call=[&](std::uint64_t peer,const std::string& route,const std::string& payload="") {
            std::optional<fsnet::Result> result;
            server.call(peer,route,payload,1s,[&](fsnet::Result value) { result=std::move(value); });
            until([&]{return result.has_value();}); if(result->status!=fsnet::Status::ok) throw std::runtime_error(route+"/"+payload+": "+authority.last_error()+" / "+ae.last_error()+" / "+(be?be->last_error():"")); return result->data;
        };
        until([&]{return !peers.empty() && entities->handle("Player","alice").has_value();});
        until([&]{return call(peers[0],"inspect")=="Player/alice:1:0:1:0:0:0:-1";});
        auto b=bob.connect("127.0.0.1",port); bh=std::make_shared<fsnet::ClientScriptHost>(bob,b);
        be=std::make_unique<fsnet::LuaEngine>(bh); be->load("local account='bob'; local peer="+std::to_string(b)+"\n"+shared+client_script);
        until([&]{return peers.size()==2 && call(peers[0],"inspect")=="Player/alice:1:1:1:0:0:0:0";});
        until([&]{return call(peers[1],"inspect")=="Player/bob:1:1:1:0:0:0:0";});
        auto intrusion=intruder.connect("127.0.0.1",alice.listen("127.0.0.1",0));
        until([&]{return intruder.connected(intrusion);});
        CHECK(intruder.notify(intrusion,"system.entities.presence","malformed"));
        for(int i=0;i<20;++i) { pump(); std::this_thread::sleep_for(1ms); }
        CHECK(alice.connected(a)); // unrelated peers cannot inject automatic entities
        CHECK(call(peers[0],"command","step").empty());
        until([&]{return call(peers[0],"inspect")=="Player/alice:1:1:1:0:0:1:0" && call(peers[1],"inspect")=="Player/bob:1:1:1:0:0:0:1";});
        CHECK(call(peers[0],"command","restore").empty());
        CHECK(call(peers[0],"command","switch").empty());
        until([&]{return call(peers[0],"inspect")=="Drone/alice:2:1:2:1:0:1:0";});
        CHECK(call(peers[0],"command","limits").empty());
        CHECK(!entities->handle("Player","missing") && entities->active("Player","extra"));
        CHECK(call(peers[0],"command","back").empty());
        until([&]{return call(peers[0],"inspect")=="Player/alice:2:1:3:2:1:1:0";});
        CHECK(call(peers[0],"command","hide").empty());
        until([&]{return call(peers[0],"inspect")=="Player/alice:2:1:3:2:2:1:-1";});
        CHECK(entities->handle("Player","bob"));
        CHECK(call(peers[0],"command","unauthorized")=="denied");
        CHECK(call(peers[0],"command","show").empty());
        until([&]{return call(peers[0],"inspect")=="Player/alice:2:2:3:2:2:1:0";});
        accounts[peers[0]]="mallory"; // all Alice grants must be revoked
        until([&]{return call(peers[0],"inspect")=="-:2:2:3:3:4:-1:-1";});
        CHECK(ae.last_error().empty() && be->last_error().empty() && authority.last_error().empty());
        // Public transport cannot invoke the internal framework spawn method.
        std::optional<fsnet::Result> forbidden;
        alice.call(a,"Player.$spawn","forged",1s,[&](fsnet::Result value) { forbidden=std::move(value); });
        until([&]{return forbidden.has_value();}); CHECK(forbidden->status==fsnet::Status::not_found);
        // Stopping automatic entities preserves manual owner subscriptions and
        // unsubscribes any automatic create that was already in flight.
        std::uint64_t manual=0;
        ah->watch_attributes("Player","mallory",[&](fsnet::Status status,std::uint64_t id) { CHECK(status==fsnet::Status::ok); manual=id; },1s);
        until([&]{return manual!=0;});
        ah->stop_entity_events();
        std::optional<fsnet::Result> late;
        replication.visibility(peers[0],"Player","extra",1,[&](fsnet::Result value) { late=std::move(value); });
        until([&]{return late.has_value();}); CHECK(late->status==fsnet::Status::ok);
        for(int i=0;i<30;++i) { pump(); std::this_thread::sleep_for(1ms); }
        CHECK(!ah->pop_entity_event() && ah->attribute_snapshot(manual));
        CHECK(call(peers[1],"arm").empty());
        CHECK(call(peers[1],"command","fault").empty());
        until([&]{return be->last_error().find("factory failure")!=std::string::npos;});
        auto cleaned=call(peers[1],"inspect");
        CHECK(cleaned.starts_with("-:") && cleaned.ends_with(":-1:-1"));
        ae.stop(); CHECK(ae.memory_used()==0); ah->stop(); be->stop(); CHECK(be->memory_used()==0); bh->stop(); authority.stop();
        std::cout<<"PASS entity presence: login spawn/restore, automatic local/remote factories, privacy, possession, hide/show and identity revocation\n";
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}

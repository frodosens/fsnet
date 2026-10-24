#include "fsnet/lua_engine.hpp"
#include "fsnet/entity_host.hpp"
#include "fsnet/entity_message.hpp"
#include "fsnet/attribute_replication.hpp"
#include <chrono>
#include <iostream>
#include <optional>
#include <thread>
using namespace std::chrono_literals;
#define CHECK(x) do { if (!(x)) throw std::runtime_error(std::string(__FILE__)+':'+std::to_string(__LINE__)+" " #x); } while(false)
void integration() {
    fsnet::Node server("test"), logic("test"), client("test"), backend("test");
    fsnet::Reply held;
    backend.bind_rpc("echo",{true,true,false},[&](const fsnet::CallIdentity&,std::string data,fsnet::Reply reply) {
        if(data=="wait") held=std::move(reply); else reply({fsnet::Status::ok,std::move(data)});
    });
    auto backend_port=backend.listen("127.0.0.1",0), port=server.listen("127.0.0.1",0);
    // Explicit loopback fixture identities, not a production login policy.
    std::uint64_t client_in=0;
    server.identify([&](std::uint64_t peer,const std::string&) {
        return std::optional(fsnet::CallIdentity{peer==client_in?fsnet::Caller::client:fsnet::Caller::server,peer==client_in?"alice":"logic"});
    });
    server.authorize([&](std::uint64_t peer,const std::string&,const std::string&) { client_in=peer; return true; });
    auto client_peer=client.connect("127.0.0.1",port); bool connected=false;
    client.call(client_peer,"system.health","",1s,[&](fsnet::Result r) { CHECK(r.status==fsnet::Status::ok); connected=true; });
    auto deadline=std::chrono::steady_clock::now()+2s;
    while(!connected) { server.poll(); client.poll(); CHECK(std::chrono::steady_clock::now()<deadline); std::this_thread::sleep_for(1ms); }
    server.authorize({}); auto logic_peer=logic.connect("127.0.0.1",port);
    auto transport=std::make_shared<fsnet::NodeScriptHost>(server);
    auto entities=std::make_shared<fsnet::EntityScriptHost>(transport,fsnet::memory_database(),"objects/boot");
    fsnet::LuaEngine engine(entities);
    std::string source="local backend_port="+std::to_string(backend_port)+"; local owner_port="+std::to_string(port)+"\n"+R"(
        local Entity=fsnet.Entity
        local Combatant=Entity:extend('Combatant')
        Combatant:attr('hp',{type='int32',default=100,sync='observers'})
        Combatant:rpc('damage',{callers={'server'}})
        function Combatant:is_alive() return self.hp>0 end
        function Combatant:damage(payload)
            assert(self:is_alive())
            local amount=tonumber(payload)
            if not amount or amount<0 then self:reject() end
            self.hp=math.max(0,self.hp-amount)
            return tostring(self.hp)
        end
        local Player=Combatant:extend('Player',{owner='id'})
        Player:attr('label',{type='string',default='Player',client='rw',sync='owner'})
        Player:rpc('damage',{callers={'server'}})
        function Player:damage(payload)
            assert(self.type=='Player' and self.context.entity_id==self.id and self.context.caller=='server')
            return Combatant.damage(self,payload)
        end
        Player:rpc('read',{callers={'client'},access='owner'})
        function Player:read() return self.type..'/'..self.id..':'..self.hp end
        function Player:validate_write(proposal) return not proposal.label or #proposal.label<=8 end
        local saved,peer,owner_peer
        Player:rpc('hold',{callers={'server'}})
        function Player:hold()
            saved=self
            peer=peer or runtime.connect('127.0.0.1',backend_port)
            local value=runtime.request(peer,'echo','wait',1000)
            assert(self.id=='alice' and self.hp==95)
            self.label=value
        end
        Player:rpc('probe',{callers={'server'}})
        function Player:probe()
            assert(not pcall(function() return saved.hp end))
            assert(not pcall(function() saved.hp=0 end))
            assert(not pcall(function() saved:is_alive() end))
            assert(not pcall(function() rawset(self,'hp',0) end))
            return tostring(self.hp)
        end
        Player:rpc('relay',{callers={'server'}})
        function Player:relay(id)
            owner_peer=owner_peer or runtime.connect('127.0.0.1',owner_port)
            return Player:ref(id,{peer=owner_peer}):damage('3',{timeout_ms=1000,request_id='ref-1'})
        end
        Player:rpc('explode',{callers={'server'}})
        function Player:explode() saved=self; self.hp=0; error('abort object mutation') end
        Player:rpc('cycle',{callers={'server'}})
        function Player:cycle() return Player:ref(self.id):damage('1') end
        Player:register()
        local Npc=Combatant:extend('Npc')
        Npc:register()
        local Private=Combatant:extend('Private')
        Private:private('damage')
        Private:register()
        local Ticker=Entity:extend('Ticker')
        Ticker:attr('ticks',{type='int32',default=0})
        Ticker:rpc('read',{callers={'server'}})
        function Ticker:read() return tostring(self.ticks) end
        function Ticker:on_tick(step)
            assert(math.type(step)=='integer' and step>0 and self.context.caller=='server')
            self.ticks=self.ticks+1
        end
        Ticker:register()
    )";
    engine.load(source);
    fsnet::AttributeReplication replication(server,entities);
    fsnet::AttributeClient projection(client,client_peer);
    auto pump=[&] { server.poll(); logic.poll(); client.poll(); backend.poll(); entities->poll(); engine.poll(); replication.poll(); projection.poll(); };
    auto until=[&](auto done) { auto end=std::chrono::steady_clock::now()+3s; while(!done()) { pump(); CHECK(std::chrono::steady_clock::now()<end); std::this_thread::sleep_for(1ms); } };
    auto call=[&](fsnet::Node& caller,std::uint64_t peer,const std::string& kind,const std::string& id,const std::string& method,const std::string& data="") {
        std::optional<fsnet::Result> result;
        caller.call(peer,kind+'.'+method,fsnet::entity_message(id,data),2s,[&](fsnet::Result r) { result=std::move(r); });
        until([&] { return result.has_value(); }); return *result;
    };
    std::uint64_t subscription=0; std::optional<fsnet::Status> watched;
    projection.watch("Player","alice",[&](fsnet::Status status,std::uint64_t id) { watched=status; subscription=id; });
    until([&] { return watched.has_value(); }); CHECK(*watched==fsnet::Status::ok);
    CHECK(call(client,client_peer,"Player","alice","read").data=="Player/alice:100");
    CHECK(call(client,client_peer,"Player","bob","read").status==fsnet::Status::rejected);
    CHECK(call(client,client_peer,"Player","alice","damage","5").status==fsnet::Status::rejected);
    CHECK(call(logic,logic_peer,"Player","alice","damage","5").data=="95");
    until([&] { return std::get<std::int64_t>(projection.get(subscription)->values().at("hp"))==95; });
    CHECK(call(logic,logic_peer,"Npc","one","damage","2").data=="98");
    CHECK(!server.has_route("Player.is_alive") && !server.has_route("Private.damage"));
    CHECK(call(logic,logic_peer,"Player","alice","damage","-1").status==fsnet::Status::rejected);
    std::optional<fsnet::Result> waiting;
    logic.call(logic_peer,"Player.hold",fsnet::entity_message("alice",""),2s,[&](fsnet::Result r) { waiting=std::move(r); });
    until([&] { return bool(held); });
    CHECK(call(logic,logic_peer,"Player","bob","probe").data=="100"); // active object cannot cross task boundaries
    held({fsnet::Status::ok,"awake"}); until([&] { return waiting.has_value(); }); CHECK(waiting->status==fsnet::Status::ok);
    CHECK(call(logic,logic_peer,"Player","alice","probe").data=="95"); // escaped object is closed after return
    std::optional<fsnet::Status> written;
    projection.write(subscription,{{"label",std::string("ok")}},[&](fsnet::Status s) { written=s; });
    until([&] { return written.has_value(); }); CHECK(*written==fsnet::Status::ok);
    written.reset(); projection.write(subscription,{{"label",std::string("too-long-name")}},[&](fsnet::Status s) { written=s; });
    until([&] { return written.has_value(); }); CHECK(*written==fsnet::Status::rejected);
    CHECK(call(logic,logic_peer,"Player","alice","relay","target").data=="97");
    CHECK(call(logic,logic_peer,"Player","alice","relay","target").data=="97"); // reference request ID replay
    CHECK(call(logic,logic_peer,"Player","alice","cycle").status==fsnet::Status::internal);
    CHECK(call(logic,logic_peer,"Player","alice","explode").status==fsnet::Status::internal);
    CHECK(call(client,client_peer,"Player","alice","read").data=="Player/alice:95");
    CHECK(call(logic,logic_peer,"Player","alice","probe").data=="95");
    CHECK(call(logic,logic_peer,"Ticker","clock","read").data=="0");
    auto ticking=std::chrono::steady_clock::now()+200ms;
    while(std::chrono::steady_clock::now()<ticking) { pump(); std::this_thread::sleep_for(1ms); }
    CHECK(std::stoi(call(logic,logic_peer,"Ticker","clock","read").data)>=1 && !server.has_route("Ticker.$tick"));

    engine.stop(); CHECK(engine.memory_used()==0);
    std::cout<<"PASS OO Lua: inheritance, base/local methods, explicit RPCs, native properties, references, scopes, validation and synchronization\n";
}
void invalid_definitions() {
    for(const auto& source : {
        "local A=fsnet.Entity:extend('A'); A:rpc('hit',{callers={'client'}}); function A:hit() end; local B=A:extend('B'); function B:hit() end; B:register()",
        "local A=fsnet.Entity:extend('A'); A:rpc('missing',{callers={'server'}}); A:register()",
        "local A=fsnet.Entity:extend('A'); A:attr('hp',{type='int32',default=1}); function A:hp() end; A:register()",
        "local A=fsnet.Entity:extend('A'); A:attr('secret',{type='string',default='',clinet='none'})",
        "local A=fsnet.Entity:extend('A'); function A:init() end",
        "local A=fsnet.Entity:extend('A'); A:private('typo'); A:register()",
        "local A=fsnet.Entity:extend('A'); A:rpc('hit',{callers={'client','client'}})",
        "local A=fsnet.Entity:extend('A'); function A:hit() end; A:register(); function A:another() end",
        "local A=fsnet.Entity:extend('A',{unrecognized=true})",
    }) {
        fsnet::Node node("test"); auto transport=std::make_shared<fsnet::NodeScriptHost>(node);
        auto host=std::make_shared<fsnet::EntityScriptHost>(transport,fsnet::memory_database(),"invalid/boot"); fsnet::LuaEngine engine(host);
        bool failed=false; try { engine.load(source); } catch(const std::exception&) { failed=true; }
        CHECK(failed && engine.memory_used()==0 && !node.has_route("A.hit") && !node.has_route("B.hit"));
    }
    std::cout<<"PASS OO definitions: policy override safety, collision/typo detection, unsupported hooks and sealed classes\n";
}
void registration_rollback() {
    fsnet::Node server("test"), client("test");
    auto transport=std::make_shared<fsnet::NodeScriptHost>(server);
    auto host=std::make_shared<fsnet::EntityScriptHost>(transport,fsnet::memory_database(),"rollback/boot");
    fsnet::ScriptLimits limits; limits.registered_routes=3; fsnet::LuaEngine engine(host,limits);
    engine.load(R"(
        local A=fsnet.Entity:extend('A')
        A:attr('value',{type='int32',default=1})
        A:rpc('one',{callers={'client'}}); function A:one() end
        A:rpc('two',{callers={'client'}}); function A:two() end
        A:rpc('three',{callers={'client'}}); function A:three() end
        assert(not pcall(function() A:register() end)) -- plus the built-in property write route exceeds the budget
        runtime.rpc('healthy',{callers={'client'},handler=function(ctx,payload) return payload end})
    )");
    CHECK(!server.has_route("A.one") && !server.has_route("A.two") && !server.has_route("A.three") && !host->schema("A"));
    auto peer=client.connect("127.0.0.1",server.listen("127.0.0.1",0)); std::optional<fsnet::Result> result;
    client.call(peer,"healthy","ok",1s,[&](fsnet::Result r) { result=std::move(r); });
    auto deadline=std::chrono::steady_clock::now()+2s;
    while(!result) { server.poll(); client.poll(); engine.poll(); CHECK(std::chrono::steady_clock::now()<deadline); std::this_thread::sleep_for(1ms); }
    CHECK(result->status==fsnet::Status::ok && result->data=="ok");
    std::cout<<"PASS OO registration: caught partial registration failure leaves no schemas, routes or consumed capacity\n";
}
int main() { try { integration(); invalid_definitions(); registration_rollback(); } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; } }

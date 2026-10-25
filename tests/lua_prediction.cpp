#include "fsnet/lua_engine.hpp"
#include "fsnet/entity_host.hpp"
#include "fsnet/client_script_host.hpp"
#include <chrono>
#include <iostream>
#include <optional>
#include <unordered_map>
#include <thread>
using namespace std::chrono_literals;
#define CHECK(x) do { if(!(x)) throw std::runtime_error(std::string(__FILE__)+':'+std::to_string(__LINE__)+" " #x); } while(false)
const char* model=R"(
    local Actor=fsnet.Entity:extend('Actor',{owner='legacy_id'})
    Actor:attr('x',{type='int32',default=100,authority='driver',sync='owner'})
    Actor:attr('ack',{type='int32',default=0,sync='owner'})
    Actor:attr('secret',{type='int32',default=42,client='none',sync='none'})
    local replay_fail=false
    local other
    function Actor:move(payload)
        self.x=self.x+assert(tonumber(payload))
        if replay_fail and self.x>=110 then
            pcall(runtime.sleep,1) -- swallowing the error must not accept replay
        end
        return 'moved:'..payload
    end
    function Actor:bad(payload)
        self.x=999
        if payload=='sleep' then pcall(runtime.sleep,1)
        elseif payload=='connect' then pcall(runtime.connect,'127.0.0.1',1234)
        elseif payload=='rpc' then pcall(runtime.request,1,'forbidden','',100)
        elseif payload=='watch' then pcall(Actor.watch,Actor,'alice')
        elseif payload=='write' then pcall(self.write,self,{x=999})
        elseif payload=='close' then pcall(self.close,self)
        elseif payload=='print' then pcall(print,'forbidden')
        elseif payload=='random' then pcall(math.random)
        elseif payload=='other' then pcall(function() other.x=999 end)
        elseif payload=='private' then self.secret=1
        elseif payload=='authority' then self.ack=1
        elseif payload=='oom' then return string.rep('x',64*1024*1024)
        elseif payload=='error' then error('bad input')
        elseif payload=='return' then return {} end
    end
    Actor:rpc('correct',{callers={'client'},access='owner'})
    function Actor:correct(payload)
        local seq,delta=payload:match('(%d+):(%d+)')
        self.x=self.x+tonumber(delta); self.ack=tonumber(seq)
        return ''
    end
)";
struct PredictionHost final : fsnet::ScriptHost {
    std::unordered_map<std::string,fsnet::Handler> handlers;
    unsigned calls=0;
    std::shared_ptr<const fsnet::AttributeUpdate> snapshot=std::make_shared<const fsnet::AttributeUpdate>(
        fsnet::AttributeUpdate{0,0,true,{{"x",std::int64_t(0)}}});
    bool has_route(const std::string& route) const override { return handlers.contains(route); }
    void bind(std::string route,fsnet::Handler handler) override { handlers.emplace(std::move(route),std::move(handler)); }
    void unbind(const std::string& route) override { handlers.erase(route); }
    std::uint64_t connect(const std::string&,std::uint16_t) override { return 1; }
    void call(std::uint64_t,std::string,std::string data,std::chrono::milliseconds,fsnet::Reply reply) override {
        ++calls; reply({fsnet::Status::ok,std::move(data)});
    }
    void watch_attributes(std::string,std::string,AttributeReady ready,std::chrono::milliseconds) override { ready(fsnet::Status::ok,1); }
    std::shared_ptr<const fsnet::AttributeUpdate> attribute_snapshot(std::uint64_t) const override { return snapshot; }
};
void prediction_budget() {
    auto host=std::make_shared<PredictionHost>();
    fsnet::ScriptLimits limits; limits.instructions_per_resume=10000;
    fsnet::LuaEngine engine(host,limits);
    engine.load(R"(
        local C=fsnet.Entity:extend('Budget')
        C:attr('x',{type='int32',default=0,authority='driver'})
        function C:spin()
            self.x=999
            while true do pcall(function() while true do end end) end
        end
        function C:advance() self.x=self.x+1 end
        C:define()
        local prediction
        runtime.start(function() prediction=(C:legacy_watch('alice')):prediction() end)
        runtime.legacy_rpc('spin',function()
            pcall(prediction.push,prediction,1,'spin','')
            return 'must not succeed'
        end)
        runtime.legacy_rpc('healthy',function()
            assert(prediction.pending==0 and prediction.state.x==0)
            prediction:push(1,'advance','')
            assert(prediction.state.x==1)
            return runtime.request(1,'echo','healthy')
        end)
    )");
    for(int i=0;i<10;++i) engine.poll();
    auto invoke=[&](const std::string& route) {
        std::optional<fsnet::Result> result;
        host->handlers.at(route)("",[&](fsnet::Result value) { result=std::move(value); });
        for(int i=0;i<100 && !result;++i) engine.poll();
        CHECK(result); return *result;
    };
    CHECK(invoke("spin").status==fsnet::Status::overloaded);
    CHECK(invoke("healthy").data=="healthy" && host->calls==1);
    engine.stop(); CHECK(engine.memory_used()==0);
    std::cout<<"PASS prediction budget: uncatchable exhaustion, unchanged state and restored effect permissions\n";
}
int main() {
    try {
        prediction_budget();
        fsnet::Node server("prediction"),client("prediction");
        std::string account="alice",report;
        unsigned forbidden=0;
        server.identify([&](std::uint64_t,const std::string&) { return std::optional(fsnet::CallIdentity{fsnet::Caller::client,account}); });
        server.bind("report",[&](std::string data,fsnet::Reply reply) { report=std::move(data); reply({fsnet::Status::ok,{}}); });
        server.bind("forbidden",[&](std::string,fsnet::Reply reply) { ++forbidden; reply({fsnet::Status::ok,{}}); });
        auto transport=std::make_shared<fsnet::NodeScriptHost>(server);
        auto entities=std::make_shared<fsnet::EntityScriptHost>(transport,fsnet::memory_database(),"prediction/boot");
        fsnet::AttributeReplication replication(server,entities);
        fsnet::LuaEngine authority(entities); authority.load(std::string(model)+"\nActor:register()\n");
        auto peer=client.connect("127.0.0.1",server.listen("127.0.0.1",0));
        auto host=std::make_shared<fsnet::ClientScriptHost>(client,peer);
        fsnet::LuaEngine frontend(host);
        frontend.load("local peer="+std::to_string(peer)+"\n"+model+R"(
            Actor:define()
            runtime.start(function()
                local confirmed=Actor:legacy_watch('alice')
                other=Actor:spawn_local({x=10})
                assert(not pcall(other.prediction,other))
                assert(not pcall(confirmed.prediction,confirmed,{max_inputs=129}))
                local prediction=confirmed:prediction({max_inputs=2,max_bytes=64})
                assert(prediction.state.x==100 and prediction.state.mode=='prediction')
                assert(not pcall(function() prediction.state.x=0 end))
                assert(not pcall(function() return prediction.state.secret end))
                for _,bad in ipairs({'sleep','connect','rpc','watch','write','close','print','random','other','private','authority','oom','error','return'}) do
                    assert(not pcall(prediction.push,prediction,1,'bad',bad),bad)
                    assert(prediction.state.x==100 and confirmed.x==100 and prediction.pending==0 and other.x==10,bad)
                end
                local effects=0
                local function input(seq,value)
                    assert(prediction:push(seq,'move',value)=='moved:'..value)
                    effects=effects+1
                end
                local obsolete=prediction.state
                input(1,'5'); input(2,'7')
                assert(obsolete.x==100) -- retained read-only views never change with the controller
                assert(prediction.state.x==112 and confirmed.x==100 and prediction.pending==2)
                assert(not pcall(prediction.push,prediction,2,'move','7'))
                assert(not pcall(prediction.push,prediction,3,'move','1'))
                Actor:legacy_ref('alice',{peer=peer}):correct('1:3')
                while confirmed.ack<1 do runtime.sleep(1) end
                assert(confirmed.x==103 and prediction.state.x==112)
                assert(not pcall(prediction.reconcile,prediction,1,confirmed.revision+1))
                replay_fail=true
                assert(not pcall(prediction.reconcile,prediction,1,confirmed.revision))
                assert(prediction.state.x==112 and prediction.pending==2 and prediction.acknowledged==0)
                replay_fail=false
                prediction:reconcile(1,confirmed.revision)
                assert(prediction.state.x==110 and confirmed.x==103 and prediction.pending==1 and effects==2)
                prediction:reconcile(1,confirmed.revision) -- duplicate confirmation is harmless
                assert(prediction.state.x==110 and effects==2)
                Actor:legacy_ref('alice',{peer=peer}):correct('2:4')
                while confirmed.ack<2 do runtime.sleep(1) end
                prediction:reconcile(2,confirmed.revision)
                assert(prediction.state.x==107 and prediction.pending==0 and prediction.acknowledged==2 and effects==2)
                assert(not pcall(prediction.reconcile,prediction,1,confirmed.revision))
                local resumed=confirmed:prediction({acknowledged=2})
                resumed:push(3,'move','1')
                assert(resumed.state.x==108 and resumed.acknowledged==2)
                resumed:close()
                assert(confirmed.x==107) -- closing a predictor retains its subscription
                assert(not pcall(prediction.push,prediction,3,'move',string.rep('1',65)))
                input(3,'1')
                assert(prediction.state.x==108 and confirmed.x==107)
                runtime.request(peer,'report','ready')
                while pcall(function() return confirmed.x end) do runtime.sleep(1) end
                assert(not pcall(function() return prediction.state.x end))
                prediction:close(); prediction:close(); confirmed:close(); other:close()
                assert(not pcall(function() prediction:push(4,'move','1') end))
                runtime.request(peer,'report','done')
            end)
        )");
        auto until=[&](auto done) {
            auto deadline=std::chrono::steady_clock::now()+5s;
            while(!done()) {
                server.poll(); client.poll(); entities->poll(); replication.poll(); host->poll(); authority.poll(); frontend.poll();
                if(std::chrono::steady_clock::now()>deadline) throw std::runtime_error("deadline: "+frontend.last_error()+" / "+authority.last_error());
                std::this_thread::sleep_for(1ms);
            }
        };
        until([&]{return report=="ready";});
        CHECK(forbidden==0);
        account="mallory";
        until([&]{return report=="done" && frontend.active()==0;});
        CHECK(frontend.last_error().empty() && forbidden==0);
        frontend.stop(); CHECK(frontend.memory_used()==0); host->stop(); authority.stop();
        std::cout<<"PASS prediction: authoritative correction, pending replay, effect isolation, rollback, limits and revocation\n";
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}

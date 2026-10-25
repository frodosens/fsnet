#include "fsnet/lua_engine.hpp"
#include "fsnet/entity_host.hpp"
#include "fsnet/entity_message.hpp"
#include "fsnet/client_script_host.hpp"
#include <chrono>
#include <iostream>
#include <thread>
using namespace std::chrono_literals;
#define CHECK(x) do { if(!(x)) throw std::runtime_error(std::string(__FILE__)+':'+std::to_string(__LINE__)+" " #x); } while(false)
const char* shared=R"(
    local Circle=fsnet.Entity:extend('Circle',{owner='legacy_id'})
    Circle:attr('hp',{type='int32',default=100,authority='driver',client='r',sync='observers'})
    Circle:attr('label',{type='string',default='initial',client='rw',sync='owner'})
    Circle:attr('secret',{type='int32',default=42,client='none',sync='none'})
    function Circle:advance(amount) self.hp=self.hp+amount end
    Circle:method('server_only',{runs_on='server'})
    function Circle:server_only() return 'server' end
    Circle:method('client_only',{runs_on='client'})
    function Circle:client_only() return 'client' end
    Circle:rpc('step',{callers={'client'},access='owner'})
    function Circle:step(payload)
        assert(self:server_only()=='server' and not pcall(self.client_only,self))
        self:advance(assert(tonumber(payload)))
        return tostring(self.hp)
    end
    function Circle:validate_write(values) return not values.label or #values.label<=8 end
)";
int main() {
    try {
        fsnet::Node server("shared"),client("shared");
        std::string account="alice";
        server.identify([&](std::uint64_t,const std::string&) { return std::optional(fsnet::CallIdentity{fsnet::Caller::client,account}); });
        auto transport=std::make_shared<fsnet::NodeScriptHost>(server);
        auto entities=std::make_shared<fsnet::EntityScriptHost>(transport,fsnet::memory_database(),"shared/boot");
        fsnet::AttributeReplication replication(server,entities);
        fsnet::LuaEngine authority(entities); authority.load(std::string(shared)+"\nCircle:register()\n");
        auto peer=client.connect("127.0.0.1",server.listen("127.0.0.1",0));
        auto host=std::make_shared<fsnet::ClientScriptHost>(client,peer);
        fsnet::LuaEngine frontend(host);
        std::string report;
        server.bind("report",[&](std::string data,fsnet::Reply reply) { report=std::move(data); reply({fsnet::Status::ok,{}}); });
        frontend.load(std::string("local peer=")+std::to_string(peer)+"\n"+shared+R"(
            Circle:define()
            local local_circle=Circle:spawn_local({hp=10},{id='local'})
            local_circle:advance(2)
            assert(local_circle.hp==12 and local_circle.mode=='local' and local_circle.id=='local')
            assert(local_circle:client_only()=='client' and not pcall(local_circle.server_only,local_circle))
            assert(not pcall(function() local_circle.label='denied' end))
            assert(not pcall(function() local_circle.hp=1.5 end))
            assert(not pcall(rawset,local_circle,'hp',999))
            local_circle:close()
            assert(not pcall(function() return local_circle.hp end))
            runtime.start(function()
                local circle=Circle:legacy_watch('alice')
                assert(circle.hp==100 and circle.mode=='replica' and circle.type=='Circle')
                assert(not pcall(function() return circle.secret end))
                assert(not pcall(function() circle.hp=0 end))
                assert(not pcall(circle.advance,circle,1))
                assert(circle:client_only()=='client')
                assert(not pcall(circle.server_only,circle))
                circle:write({label='ok'},{request_id='label-once'})
                while circle.label~='ok' do runtime.sleep(1) end
                assert(not pcall(function() circle:write({hp=0}) end))
                assert(not pcall(function() circle:write({label='too-long-name'}) end))
                runtime.request(peer,'report','ready')
                while circle.hp==100 do runtime.sleep(1) end
                assert(circle.hp==105)
                runtime.request(peer,'report','updated')
                while pcall(function() return circle.hp end) do runtime.sleep(1) end
                circle:close()
                runtime.request(peer,'report','done')
            end)
        )");
        CHECK(!client.has_route("Circle.step") && !client.has_route("Circle.$write"));
        auto pump=[&] { server.poll(); client.poll(); entities->poll(); replication.poll(); host->poll(); authority.poll(); frontend.poll(); };
        auto until=[&](auto done) {
            auto deadline=std::chrono::steady_clock::now()+5s;
            while(!done()) {
                pump();
                if(std::chrono::steady_clock::now()>deadline) throw std::runtime_error("deadline: "+frontend.last_error()+" / "+authority.last_error());
                std::this_thread::sleep_for(1ms);
            }
        };
        until([&]{return report=="ready";});
        bool stepped=false;
        client.call(peer,"Circle.step",fsnet::entity_message("alice","5"),1s,[&](fsnet::Result result) { CHECK(result.status==fsnet::Status::ok && result.data=="105"); stepped=true; });
        until([&]{return stepped && report=="updated";});
        account="mallory";
        until([&]{return report=="done";});
        until([&]{return frontend.active()==0;});
        CHECK(frontend.last_error().empty());
        frontend.stop(); CHECK(frontend.memory_used()==0); host->stop(); authority.stop();
        std::cout<<"PASS shared Lua: local carriers, execution domains, native watch/write, read-only replicas, timers and revocation\n";
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}

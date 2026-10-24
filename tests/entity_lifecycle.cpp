#include "fsnet/lua_engine.hpp"
#include "fsnet/entity_host.hpp"
#include "fsnet/entity_message.hpp"
#include "fsnet/entity_record.hpp"
#include <iostream>
#include <thread>
using namespace std::chrono_literals;
#define CHECK(x) do { if(!(x)) throw std::runtime_error(std::string(__FILE__)+':'+std::to_string(__LINE__)+" " #x); } while(false)
const char* model=R"(
    local Life=fsnet.Entity:extend('Life',{owner='id'})
    Life:attr('created',{type='int32',default=0})
    Life:attr('activated',{type='int32',default=0})
    Life:attr('deactivated',{type='int32',default=0})
    Life:attr('value',{type='int32',default=0})
    local fail_create,fail_deactivate=false,false
    function Life:on_create()
        self.created=self.created+1; self.value=10
        if fail_create then error('create failed') end
    end
    function Life:on_activate() self.activated=self.activated+1 end
    function Life:on_deactivate(reason)
        assert(reason=='explicit' or reason=='idle' or reason=='destroy')
        self.deactivated=self.deactivated+1; self.value=self.value+1
        if fail_deactivate then error('deactivate failed') end
    end
    function Life:on_destroy(reason) assert(reason=='destroy') end
    Life:rpc('read',{callers={'server'}})
    function Life:read() return self.created..':'..self.activated..':'..self.deactivated..':'..self.value end
    Life:rpc('fail',{callers={'server'}})
    function Life:fail() self.value=999; error('abort') end
    Life:register()
    runtime.rpc('config',{callers={'server'},handler=function(ctx,value) fail_create=value=='fail'; fail_deactivate=value=='deactivate-fail'; return '' end})
    runtime.rpc('deactivate',{callers={'server'},handler=function(ctx,id) Life:deactivate(id); return '' end})
    runtime.rpc('destroy',{callers={'server'},handler=function(ctx,id) Life:destroy(id); return '' end})
)";
int main() {
    try {
        fsnet::Node server("life"),client("life");
        server.identify([](std::uint64_t,const std::string&){return std::optional(fsnet::CallIdentity{fsnet::Caller::server,"test"});});
        auto db=fsnet::memory_database(); std::int64_t now=1000;
        fsnet::EntityRuntimeLimits limits; limits.lease=2s; limits.request_timeout=1s; limits.idle_timeout=1h;
        auto transport=std::make_shared<fsnet::NodeScriptHost>(server);
        auto host=std::make_shared<fsnet::EntityScriptHost>(transport,db,"node/first",limits,fsnet::StorageLimits{},[&]{return now;});
        auto engine=std::make_unique<fsnet::LuaEngine>(host); engine->load(model);
        CHECK(!server.has_route("Life.$create") && !server.has_route("Life.$activate") && !server.has_route("Life.$deactivate") && !server.has_route("Life.$destroy"));
        auto port=server.listen("127.0.0.1",0); auto peer=client.connect("127.0.0.1",port);
        auto pump=[&]{server.poll();client.poll();host->poll();engine->poll();};
        auto until=[&](auto condition){auto end=std::chrono::steady_clock::now()+5s; while(!condition()){pump();CHECK(std::chrono::steady_clock::now()<end);std::this_thread::sleep_for(1ms);}};
        auto request=[&](std::string route,std::string data){std::optional<fsnet::Result> result;client.call(peer,std::move(route),std::move(data),2s,[&](auto r){result=std::move(r);});until([&]{return result.has_value();});return *result;};
        auto read=[&](std::string id="alice"){auto r=request("Life.read",fsnet::entity_message(id,""));CHECK(r.status==fsnet::Status::ok);return r.data;};
        CHECK(read()=="1:1:0:10");CHECK(read()=="1:1:0:10");
        auto first=host->handle("Life","alice");CHECK(first);
        CHECK(request("Life.fail",fsnet::entity_message("alice","")).status==fsnet::Status::internal);
        CHECK(read()=="1:1:0:10");
        CHECK(request("deactivate","alice").status==fsnet::Status::ok); CHECK(!host->handle("Life","alice"));
        auto record=fsnet::decode_entity_record(db->get(fsnet::entity_record_key("Life","alice"))->value);
        CHECK(record.initialized && record.owner.empty());
        CHECK(read()=="1:2:1:11"); CHECK(host->handle("Life","alice")->generation!=first->generation);
        std::optional<fsnet::Result> stale;host->deactivate(*first,[&](auto r){stale=std::move(r);});CHECK(stale && stale->status==fsnet::Status::rejected);
        CHECK(request("config","deactivate-fail").status==fsnet::Status::ok);
        CHECK(request("deactivate","alice").status==fsnet::Status::internal);
        CHECK(read()=="1:2:1:11");
        CHECK(request("config","").status==fsnet::Status::ok);
        auto version=db->get(fsnet::entity_record_key("Life","alice"))->version;
        CHECK(request("destroy","alice").status==fsnet::Status::ok);CHECK(!db->get(fsnet::entity_record_key("Life","alice")));
        CHECK(read()=="1:1:0:10"); CHECK(db->get(fsnet::entity_record_key("Life","alice"))->version>version);
        CHECK(request("config","fail").status==fsnet::Status::ok);
        CHECK(request("Life.read",fsnet::entity_message("pending","")).status==fsnet::Status::internal);
        auto pending=fsnet::decode_entity_record(db->get(fsnet::entity_record_key("Life","pending"))->value);CHECK(!pending.initialized);
        // Crash/stop after lease acquisition must not mark creation complete.
        engine->stop();host->stop();now+=3000;
        limits.idle_timeout=30ms;
        host=std::make_shared<fsnet::EntityScriptHost>(transport,db,"node/second",limits,fsnet::StorageLimits{},[&]{return now;});
        engine=std::make_unique<fsnet::LuaEngine>(host);engine->load(model);
        CHECK(read("pending")=="1:1:0:10");
        auto end=std::chrono::steady_clock::now()+2s;
        while(host->handle("Life","pending")){pump();CHECK(std::chrono::steady_clock::now()<end);std::this_thread::sleep_for(1ms);}
        CHECK(read("pending")=="1:2:1:11");
        std::optional<fsnet::Result> drained; host->drain([&](auto r){drained=std::move(r);}); until([&]{return drained.has_value();}); CHECK(drained->status==fsnet::Status::ok && host->stats().entities==0);
        engine->stop();CHECK(engine->memory_used()==0);host->stop();
        std::cout<<"PASS transactional lifecycle: creation marker, activation, failure rollback, deactivation, destroy, idle, drain and restart\n";
    } catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}

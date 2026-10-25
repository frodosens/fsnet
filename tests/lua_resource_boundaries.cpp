#include "fsnet/lua_engine.hpp"
#include <iostream>
#include <thread>
#include <unordered_map>
using namespace std::chrono_literals;
#define CHECK(x) do { if(!(x))throw std::runtime_error(std::string(__FILE__)+":"+std::to_string(__LINE__)+" " #x); } while(false)
struct Host:fsnet::ScriptHost {
    std::unordered_map<std::string,fsnet::Handler> handlers;
    AttributeReady pending;
    std::vector<AttributeReady> watches;
    unsigned unwatches=0;
    std::shared_ptr<const fsnet::AttributeUpdate> image;
    bool has_route(const std::string& route)const override{return handlers.contains(route);}
    void bind(std::string route,fsnet::Handler handler)override{handlers.emplace(std::move(route),std::move(handler));}
    void unbind(const std::string& route)override{handlers.erase(route);}
    std::uint64_t connect(const std::string&,std::uint16_t)override{return 1;}
    void call(std::uint64_t,std::string,std::string,std::chrono::milliseconds,fsnet::Reply reply)override{reply({fsnet::Status::ok,{}});}
    void watch_attributes(std::string,std::string,AttributeReady ready,std::chrono::milliseconds)override{pending=ready;watches.push_back(std::move(ready));}
    void unwatch_attributes(std::uint64_t id)override{CHECK(id==123||id==124);++unwatches;}
    std::shared_ptr<const fsnet::AttributeUpdate> attribute_snapshot(std::uint64_t id)const override{CHECK(id==123);return image;}
};
void watch_ownership() {
    const char* waiting=R"(
        local C=fsnet.Entity:extend('Watch'); C:attr('x',{type='string',default=''}); C:define()
        runtime.start(function() C:legacy_watch('alice') end)
    )";
    for(int mode=0;mode<3;++mode) {
        auto host=std::make_shared<Host>();fsnet::ScriptLimits limits;limits.task_timeout=50ms;
        fsnet::LuaEngine engine(host,limits);engine.load(waiting);engine.poll();CHECK(host->pending&&engine.active()==1);
        if(mode!=2)host->pending(fsnet::Status::ok,123);
        if(mode!=1){std::this_thread::sleep_for(100ms);engine.poll();CHECK(engine.active()==0);}
        if(mode==2)host->pending(fsnet::Status::ok,123);
        engine.stop();CHECK(host->unwatches==1&&engine.memory_used()==0);
    }
    for(int mode=0;mode<3;++mode) {
        auto host=std::make_shared<Host>();fsnet::ScriptLimits limits;
        if(mode==2)limits.memory_bytes=256*1024;
        if(mode!=1)host->image=std::make_shared<fsnet::AttributeUpdate>(fsnet::AttributeUpdate{0,0,true,{{"x",std::string(mode==2?65536:0,'x')}}});
        fsnet::LuaEngine engine(host,limits);
        engine.load(R"(
            local C=fsnet.Entity:extend('Watch'); C:attr('x',{type='string',default=''}); C:define()
            runtime.legacy_rpc('watch',function()
                local ok,object=pcall(C.legacy_watch,C,'alice')
                if ok then object:close();return 'adopted' end
                return 'failed'
            end)
        )");
        std::optional<fsnet::Result> result;
        host->handlers.at("watch")("",[&](auto value){result=std::move(value);});engine.poll();CHECK(host->pending);
        host->pending(fsnet::Status::ok,123);host->pending(fsnet::Status::ok,123);engine.poll();
        host->pending(fsnet::Status::ok,123);
        CHECK(result&&result->status==fsnet::Status::ok&&result->data==(mode==0?"adopted":"failed"));
        CHECK(host->unwatches==1);engine.stop();CHECK(host->unwatches==1&&engine.memory_used()==0);
    }
}
void watch_completion_budget() {
    auto host=std::make_shared<Host>();fsnet::ScriptLimits limits;limits.queued_bytes=8;
    host->image=std::make_shared<fsnet::AttributeUpdate>(fsnet::AttributeUpdate{0,0,true,{{"x",std::int64_t(0)}}});
    fsnet::LuaEngine engine(host,limits);engine.load(R"(
        local C=fsnet.Entity:extend('Watch');C:attr('x',{type='int32',default=0});C:define()
        runtime.legacy_rpc('watch',function()
            local ok,object=pcall(C.legacy_watch,C,'alice')
            if ok then object:close();return 'adopted' end
            return 'failed'
        end)
    )");
    std::vector<fsnet::Result> results;
    for(int i=0;i<2;++i)host->handlers.at("watch")("",[&](auto result){results.push_back(std::move(result));});
    engine.poll();CHECK(host->watches.size()==2);
    host->watches[0](fsnet::Status::ok,123);host->watches[1](fsnet::Status::ok,124);engine.poll();
    CHECK(results.size()==2&&results[0].status==fsnet::Status::ok&&results[0].data=="adopted"&&results[1].status==fsnet::Status::ok&&results[1].data=="failed");
    CHECK(host->unwatches==2);engine.stop();CHECK(host->unwatches==2&&engine.memory_used()==0);
}
void schema_budget() {
    auto host=std::make_shared<Host>();fsnet::ScriptLimits limits;limits.memory_bytes=8*1024*1024;
    limits.instructions_per_resume=500000; // isolate memory rejection from repeated error handling work
    fsnet::LuaEngine engine(host,limits);
    engine.load(R"(
        local denied=0
        for i=1,64 do
            local C=fsnet.Entity:extend('Memory'..i)
            C:attr('value',{type=fsnet.array(fsnet.array('int32',256),15)})
            local ok,err=pcall(C.define,C)
            if not ok then assert(string.find(err,'memory budget',1,true));denied=denied+1 end
        end
        assert(denied>0)
        -- Failed registrations must not retain a schema or its budget.
        local Bad=fsnet.Entity:extend('Bad')
        Bad:attr('x',{type='int32',default=0})
        Bad:rpc('missing',{callers={'server'}})
        assert(not pcall(Bad.register,Bad))
        Bad.missing=function()return '' end
        Bad:define()
        assert(not pcall(runtime._object_registration,function()
            runtime._attributes_define('Rollback',{x={type='int32',default=0}},nil,true)
            error('rollback registration')
        end))
        local Again=fsnet.Entity:extend('Rollback')
        Again:attr('x',{type='int32',default=0});Again:define()
    )");
    CHECK(engine.memory_used()>1024*1024&&engine.memory_used()<=limits.memory_bytes);
    engine.stop();CHECK(engine.memory_used()==0);
}
void prediction_gc() {
    auto host=std::make_shared<Host>();
    host->image=std::make_shared<fsnet::AttributeUpdate>(fsnet::AttributeUpdate{0,0,true,{{"value",fsnet::AttributeArray(15,fsnet::AttributeArray(256,std::int64_t(0)))}}});
    fsnet::LuaEngine engine(host);
    engine.load(R"(
        local C=fsnet.Entity:extend('Memory')
        C:attr('value',{type=fsnet.array(fsnet.array('int32',256),15),authority='driver'})
        function C:step() return '' end
        C:define()
        runtime.start(function()
            local confirmed=C:legacy_watch('alice')
            local prediction=confirmed:prediction()
            for i=1,16 do prediction:push(i,'step','') end
            assert(prediction.pending==16)
            confirmed:close()
            for i=1,20 do local object=C:spawn_local();object:close() end
        end)
    )");
    engine.poll();CHECK(host->pending);host->pending(fsnet::Status::ok,123);
    for(int i=0;i<10&&engine.active();++i)engine.poll();
    CHECK(engine.active()==0&&engine.last_error().empty());
    engine.stop();CHECK(host->unwatches==1&&engine.memory_used()==0);
}
void schema_result_allocation(){
    auto host=std::make_shared<Host>();fsnet::ScriptLimits limits;limits.memory_bytes=320*1024;
    fsnet::LuaEngine engine(host,limits);engine.load(R"(
        local value=string.rep('x',65536)
        local spec={x={type='string',default=value}}
        assert(not pcall(runtime._attributes_define,'Retry',spec,nil,true))
        value=nil;spec=nil
        runtime._attributes_define('Retry',{x={type='int32',default=0}},nil,true)
    )");
    engine.stop();CHECK(engine.memory_used()==0);
}
int main(){try{watch_ownership();watch_completion_budget();schema_budget();schema_result_allocation();prediction_gc();std::cout<<"PASS Lua native resources: watch ownership, schema quota, prediction and local carrier GC\n";}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}

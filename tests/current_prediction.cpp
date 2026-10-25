#include "support/runtime.hpp"
#include <iostream>
using namespace fsnet;using namespace testing;
struct PredictionHost:Parent {
    std::shared_ptr<const AttributeUpdate> image=std::make_shared<const AttributeUpdate>(AttributeUpdate{0,1,true,{{"x",std::int64_t(10)}}});
    Reply checkpoint;bool done=false,watched=false;unsigned forbidden_calls=0;
    void watch_attributes(std::string type,std::string id,AttributeReady ready,std::chrono::milliseconds)override {
        require(type=="Actor" && id==EntityId(42).str(),"prediction global watch");watched=true;ready(Status::ok,1);
    }
    std::shared_ptr<const AttributeUpdate> attribute_snapshot(std::uint64_t id)const override{return id==1 && watched?image:nullptr;}
    void unwatch_attributes(std::uint64_t)override{watched=false;}
    void call_entity(std::string route,std::string bytes,std::chrono::milliseconds,Reply reply)override {
        require(route=="@","prediction entity route");auto message=parse_entity_call_view(bytes);require(bool(message),"prediction envelope");
        if(message->method=="checkpoint")checkpoint=std::move(reply);
        else if(message->method=="done"){done=true;reply({Status::ok,{}});}
        else {++forbidden_calls;reply({Status::ok,{}});}
    }
};
int main(){try {
    auto host=std::make_shared<PredictionHost>();LuaEngine vm(host);
    vm.load(R"lua(
        local id='000000000000002a'
        local Actor=fsnet.Entity:extend('Actor')
        Actor:attr('x',{type='int32',default=0,authority='driver'})
        function Actor:step(value) self.x=self.x+tonumber(value);return 'effect' end
        function Actor:bad_wait() self.x=999;runtime.sleep(1) end
        function Actor:bad_network() self.x=999;fsnet.ref(id):call('forbidden','') end
        function Actor:bad_print() self.x=999;print('forbidden') end
        Actor:define()
        runtime.start(function()
            local confirmed=Actor:watch(id)
            local prediction=confirmed:prediction({max_inputs=4})
            assert(prediction:push(1,'step','5')=='effect')
            assert(prediction:push(2,'step','7')=='effect')
            assert(confirmed.x==10 and prediction.state.x==22 and prediction.pending==2)
            for _,name in ipairs({'bad_wait','bad_network','bad_print'}) do
                assert(not pcall(function() prediction:push(3,name,'') end))
                assert(prediction.state.x==22 and prediction.pending==2)
            end
            fsnet.ref(id):call('checkpoint','')
            assert(confirmed.x==103)
            prediction:reconcile(1,confirmed.revision)
            assert(prediction.state.x==110 and confirmed.x==103 and prediction.pending==1)
            assert(not pcall(function() prediction:reconcile(0,confirmed.revision) end))
            prediction:close();confirmed:close()
            fsnet.ref(id):call('done','')
        end)
    )lua","current-prediction");
    auto until=[&](auto condition){auto end=std::chrono::steady_clock::now()+5s;while(!condition()){vm.poll();require(vm.last_error().empty(),vm.last_error().c_str());require(std::chrono::steady_clock::now()<end,"prediction deadline");std::this_thread::sleep_for(1ms);}};
    until([&]{return bool(host->checkpoint);});
    host->image=std::make_shared<const AttributeUpdate>(AttributeUpdate{0,2,true,{{"x",std::int64_t(103)}}});
    auto resume=std::move(host->checkpoint);resume({Status::ok,{}});
    until([&]{return host->done;});require(host->forbidden_calls==0 && !host->watched,"prediction effects/subscription cleanup");
    vm.stop();std::cout<<"PASS current prediction: global watch, separate confirmation, correction/replay, failed reducer rollback and effect guards\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}

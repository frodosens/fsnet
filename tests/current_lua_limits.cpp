#include "support/runtime.hpp"
#include <iostream>
using namespace fsnet;using namespace testing;
int main(){try{
    ScriptLimits limits;limits.memory_bytes=512*1024;limits.instructions_per_resume=10000;
    Runtime runtime(R"lua(
        Actor=fsnet.Entity:extend('Actor')
        Actor:attr('x',{type='int32',default=0})
        Actor:rpc('healthy',{callers={'server'}})
        function Actor:healthy() self.x=self.x+1;return tostring(self.x) end
        Actor:rpc('spin',{callers={'server'}})
        function Actor:spin() self.x=999;while true do pcall(function() for i=1,1000000 do end end) end end
        Actor:rpc('memory',{callers={'server'}})
        function Actor:memory() self.x=999;return string.rep('x',8*1024*1024) end
        Actor:rpc('sandbox',{callers={'server'}})
        function Actor:sandbox()
            assert(io==nil and os==nil and debug==nil and package==nil and require==nil)
            assert(runtime.rpc==nil and runtime.entity==nil and runtime.entity_call==nil and runtime.entity_request==nil)
            assert(not pcall(function() Actor:ref('arbitrary-name') end))
            return 'safe'
        end
        Actor:register()
    )lua",limits);
    auto id=runtime.create("Actor","budget");
    require(runtime.call(id,"sandbox").data=="safe","sandbox surface");
    require(runtime.call(id,"spin").status==Status::overloaded,"instruction budget catchable");
    require(runtime.call(id,"healthy").data=="1","state/instructions did not recover");
    require(runtime.call(id,"memory").status!=Status::ok,"memory budget bypassed");
    auto recovered=runtime.call(id,"healthy");
    if(recovered.status!=Status::ok || recovered.data!="2")throw std::runtime_error("OOM recovery status="+std::to_string(unsigned(recovered.status))+" data="+recovered.data+" x="+std::to_string(std::get<std::int64_t>(runtime.image(id).values.at("x")))+" error="+runtime.vm->last_error());
    auto memory=runtime.vm->memory_used();require(memory<=limits.memory_bytes,"native/script memory budget");
    std::cout<<"PASS current Lua: strict API surface, global references, uncatchable instruction budget, OOM rollback and recovery\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}

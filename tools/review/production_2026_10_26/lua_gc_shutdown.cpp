#include "tests/support/runtime.hpp"
#include <iostream>
int main(){
    auto host=std::make_shared<testing::Parent>();
    fsnet::ScriptLimits limits;limits.task_timeout=std::chrono::milliseconds(10);limits.instructions_per_resume=1000;
    fsnet::LuaEngine vm(host,limits);
    vm.load("held=setmetatable({}, {__gc=function() print('GC_ENTER');while true do end end})", "gc-shutdown");
    std::cerr<<"LOAD_OK\n";
    vm.stop();
    std::cerr<<"STOP_OK\n";
}

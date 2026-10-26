#include "tests/support/runtime.hpp"
#include <sys/resource.h>
#include <iostream>
using namespace fsnet;using namespace testing;
long rss(){rusage r{};getrusage(RUSAGE_SELF,&r);return r.ru_maxrss;}
int main(int argc,char**argv){try{
    ScriptLimits limits;limits.memory_bytes=2*1024*1024;limits.instructions_per_resume=100000;
    Runtime runtime(R"lua(
        local Actor=fsnet.Entity:extend('Actor')
        Actor:attr('rows',{type=fsnet.list(fsnet.array('string',256),4)})
        Actor:rpc('wide',{callers={'server'}})
        function Actor:wide(size)
            local s=string.rep('x',tonumber(size))
            local row={} for i=1,256 do row[i]=s end
            self.rows={row,row,row,row}
            return 'ok'
        end
        Actor:register()
    )lua",limits);
    auto id=runtime.create("Actor","native-budget");auto before=rss();
    auto result=runtime.call(id,"wide",argc>1?argv[1]:"65536");
    std::cout<<"status="<<unsigned(result.status)<<" vm_limit="<<limits.memory_bytes<<" vm_used="<<runtime.vm->memory_used()<<" rss_before_kib="<<before<<" rss_peak_kib="<<rss()<<"\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}

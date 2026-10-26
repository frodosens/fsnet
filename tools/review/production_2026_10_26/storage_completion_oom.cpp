#include "fsnet/async_database.hpp"
#include <cstdlib>
#include <new>
#include <exception>
#include <execinfo.h>
#include <cstdio>
#include <chrono>
#include <thread>
thread_local bool fail_new=false;
void* operator new(std::size_t n){if(fail_new) throw std::bad_alloc();if(auto p=std::malloc(n))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p) noexcept {std::free(p);} void operator delete[](void* p) noexcept {std::free(p);}
void operator delete(void* p,std::size_t) noexcept {std::free(p);} void operator delete[](void* p,std::size_t) noexcept {std::free(p);}
int main(){std::set_terminate([]{std::fputs("terminate_after_worker_completion_allocation_failure\n",stderr);void* frames[20];int count=backtrace(frames,20);backtrace_symbols_fd(frames,count,2);std::_Exit(73);});fsnet::AsyncDatabase executor(fsnet::memory_database());for(unsigned i=0;i<32;++i)executor.submit(1,[](fsnet::Storage&){fail_new=true;},[](std::exception_ptr){});executor.stop();std::puts("drained");}

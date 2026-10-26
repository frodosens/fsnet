#include "fsnet/database.hpp"
#include <iostream>
#include <new>
#include <cstdlib>
#include <string>
bool track=false;std::size_t allocated=0,allocations=0;
void* operator new(std::size_t n){if(track){allocated+=n;++allocations;}if(auto p=std::malloc(n))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n){return ::operator new(n);}void operator delete(void*p) noexcept{std::free(p);}void operator delete[](void*p)noexcept{std::free(p);}void operator delete(void*p,std::size_t)noexcept{std::free(p);}void operator delete[](void*p,std::size_t)noexcept{std::free(p);}
int main(){for(unsigned count:{100u,500u,2000u}){auto db=fsnet::memory_database();for(unsigned i=0;i<count;++i)db->compare_exchange("key"+std::to_string(i),0,std::string(256,'p'));auto old=db->get("key0");allocated=allocations=0;track=true;db->compare_exchange("key0",old->version,"updated");track=false;std::cout<<"{\"records\":"<<count<<",\"allocated_bytes_for_one_CAS\":"<<allocated<<",\"allocations\":"<<allocations<<"}\n";}}

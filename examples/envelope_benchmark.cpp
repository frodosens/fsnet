#include "fsnet/entity_message.hpp"
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <new>
// Count ordinary allocations in this standalone, single-threaded parser fixture.
static thread_local bool counting=false;
static thread_local std::size_t allocations=0,allocated_bytes=0;
void* operator new(std::size_t n){if(void* p=std::malloc(n?n:1)){if(counting){++allocations;allocated_bytes+=n;}return p;}throw std::bad_alloc();}
void operator delete(void* p) noexcept{std::free(p);}
void operator delete(void* p,std::size_t) noexcept{std::free(p);}
namespace {
struct View{std::string_view id,payload,request;};
// Review prototype, deliberately not used by the runtime. All views borrow input.
std::optional<View> view(std::string_view in){
    if(in.size()<6||(in.substr(0,4)!="FSE1"&&in.substr(0,4)!="FSE2"))return {};
    auto length=(static_cast<unsigned char>(in[4])<<8)|static_cast<unsigned char>(in[5]);
    if(!length||length>256||in.size()<6u+length)return {};
    View result{in.substr(6,length),in.substr(6+length),{}};
    if(in.substr(0,4)=="FSE2"){
        if(result.payload.size()<2)return {};
        auto n=(static_cast<unsigned char>(result.payload[0])<<8)|static_cast<unsigned char>(result.payload[1]);
        if(!n||n>128||result.payload.size()<2u+n)return {};
        result.request=result.payload.substr(2,n);result.payload.remove_prefix(2+n);
    }
    return result;
}
template<class Parse> void run(const char* name,const std::string& input,std::size_t n,Parse parse){
    allocations=allocated_bytes=0;std::size_t checksum=0;
    auto begin=std::chrono::steady_clock::now();counting=true;
    for(std::size_t i=0;i<n;++i){auto value=parse(std::string_view(input));if(!value)throw std::runtime_error("parse failed");checksum+=value->id.size()+value->payload.size()+static_cast<unsigned char>(value->payload[0]);}
    counting=false;auto seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-begin).count();
    std::cout<<"{\"scenario\":\""<<name<<"\",\"envelope_bytes\":"<<input.size()<<",\"iterations\":"<<n<<",\"ns_per_op\":"<<seconds*1e9/n<<",\"allocations\":"<<allocations<<",\"allocated_bytes\":"<<allocated_bytes<<",\"checksum\":"<<checksum<<"}\n";
}
}
int main(){try{
#ifndef NDEBUG
    throw std::runtime_error("use a Release build");
#endif
    for(auto bytes:{128,4096,65536}){
        auto input=fsnet::entity_message("alice",std::string(bytes,'x'),"request-001");
        auto owned=fsnet::parse_entity_message(input);auto borrowed=view(input);
        if(!owned||!borrowed||owned->id!=borrowed->id||owned->payload!=borrowed->payload||owned->request_id!=borrowed->request)throw std::runtime_error("prototype mismatch");
        run("owning_parser",input,50000,fsnet::parse_entity_message);
        run("borrowed_parser_prototype",input,50000,view);
    }
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}

#include "fsnet/attribute_replication.hpp"
#include "fsnet/replication_wire.hpp"
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <new>
#include <thread>
using Clock=std::chrono::steady_clock;
using namespace std::chrono_literals;
bool tracking=false;std::size_t allocated=0,allocations=0;
void* operator new(std::size_t n){if(tracking){allocated+=n;++allocations;}if(auto p=std::malloc(n))return p;throw std::bad_alloc();}
void operator delete(void* p)noexcept{std::free(p);}
void operator delete(void* p,std::size_t)noexcept{std::free(p);}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete[](void* p)noexcept{std::free(p);}
void operator delete[](void* p,std::size_t)noexcept{std::free(p);}
fsnet::AttributeUpdate image(std::size_t width){fsnet::AttributeArray a;for(std::size_t i=0;i<width;++i)a.push_back(std::string(512,'x'));return {0,0,true,{{"large",std::move(a)},{"tick",std::int64_t(0)}}};}
void output(const char* name,std::size_t width,std::size_t n,Clock::time_point begin){tracking=false;std::cout<<"{\"scenario\":\""<<name<<"\",\"unchanged_items\":"<<width<<",\"iterations\":"<<n<<",\"ns_per_op\":"<<std::chrono::duration<double,std::nano>(Clock::now()-begin).count()/n<<",\"allocations_per_op\":"<<double(allocations)/n<<",\"allocated_bytes_per_op\":"<<double(allocated)/n<<"}\n";}
int main(){
 const std::size_t n=1000;
 for(auto width:{0u,16u,256u}){
  for(bool candidate:{false,true}){fsnet::AttributeReplica replica;replica.apply(image(width));allocated=allocations=0;auto begin=Clock::now();tracking=true;
   for(std::size_t i=1;i<=n;++i){fsnet::AttributeUpdate delta{i-1,i,false,{{"tick",std::int64_t(i)}}};if(candidate){auto next=replica;next.apply(delta);replica=std::move(next);}else replica.apply(delta);}
   output(candidate?"receive_candidate_pattern":"direct_apply",width,n,begin);
  }
  fsnet::Node server("x"),client("x");std::uint64_t remote=0;server.authorize([&](auto peer,const auto&,const auto&){remote=peer;return true;});auto initial=image(width);
  server.bind("system.attributes.watch",[&](auto,fsnet::Reply reply){reply({fsnet::Status::ok,fsnet::encode_replication_packet(1,"Actor","alice",fsnet::Status::ok,&initial,30000)});});
  auto peer=client.connect("127.0.0.1",server.listen("127.0.0.1",0));fsnet::AttributeClient attributes(client,peer);bool ready=false;
  attributes.watch("Actor","alice",[&](auto status,auto){if(status!=fsnet::Status::ok)std::abort();ready=true;});auto end=Clock::now()+5s;
  while(!ready&&Clock::now()<end){server.poll();client.poll();}if(!ready)std::abort();
  allocated=allocations=0;std::chrono::nanoseconds consumed{};
  for(std::size_t i=1;i<=n;++i){fsnet::AttributeUpdate delta{i-1,i,false,{{"tick",std::int64_t(i)}}};server.notify(remote,"system.attributes.update",fsnet::encode_replication_packet(1,"Actor","alice",fsnet::Status::ok,&delta));
   while(attributes.get(1)->revision()!=i){server.poll();auto begin=Clock::now();tracking=true;client.poll();tracking=false;consumed+=Clock::now()-begin;}
  }
  output("native_attribute_client_receive",width,n,Clock::now()-consumed);
 }
}

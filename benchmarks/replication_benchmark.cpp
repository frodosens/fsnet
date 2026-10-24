#include "fsnet/replication_wire.hpp"
#include <chrono>
#include <iostream>
using Clock=std::chrono::steady_clock;
using namespace fsnet;
template<class Function> void measure(const char* name,std::size_t n,Function fn){
    std::size_t checksum=0;auto start=Clock::now();
    for(std::size_t i=0;i<n;++i)checksum+=fn(i);
    auto seconds=std::chrono::duration<double>(Clock::now()-start).count();
    std::cout<<"{\"scenario\":\""<<name<<"\",\"iterations\":"<<n<<",\"us_per_op\":"<<seconds*1e6/n<<",\"checksum\":"<<checksum<<"}\n";
}
int main(){try{
#ifndef NDEBUG
    throw std::runtime_error("use a Release build");
#endif
    AttributeArray items;for(int i=0;i<128;++i)items.emplace_back(AttributeStruct{{"id",std::string(64,'i')},{"count",std::int64_t(i)}});
    AttributeUpdate seed{0,1,true,{{"items",items},{"payload",std::string(65536,'p')},{"counter",std::int64_t(0)}}};
    const auto encoded=encode_replication_packet(1,"Bench","alice",Status::ok,&seed,30000);
    const std::size_t rounds=2000;
    measure("forward_decode_encode",rounds,[&](auto i){auto input=encoded;auto p=decode_replication_packet(input);auto out=encode_replication_packet(i+2,p.type,p.id,p.status,&*p.update,p.lease_ms);return out.size();});
    measure("forward_validate_remap",rounds,[&](auto i){auto input=encoded;auto p=decode_replication_packet_view(input);remap_replication_subscription(input,i+2);return input.size()+p.subscription-1;});
    AttributeReplica eager,shared; if(!eager.apply(seed)||!shared.apply(seed))throw std::runtime_error("snapshot setup failed");
    measure("eager_snapshot_clone",rounds,[&](auto i){if(!eager.apply({i+1,i+2,false,{{"counter",std::int64_t(i)}}}))throw std::runtime_error("apply failed");auto image=std::make_shared<const AttributeUpdate>(AttributeUpdate{0,eager.revision(),true,eager.values()});return image->revision;});
    measure("shared_snapshot_acquire",rounds,[&](auto i){if(!shared.apply({i+1,i+2,false,{{"counter",std::int64_t(i)}}}))throw std::runtime_error("apply failed");auto image=shared.snapshot();return image->revision;});
    if(eager.revision()!=shared.revision()||eager.values()!=shared.values())throw std::runtime_error("final snapshot mismatch");
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}

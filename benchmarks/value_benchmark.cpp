#include "fsnet/attribute_wire.hpp"
#include <chrono>
#include <iostream>
using namespace fsnet;
using Clock=std::chrono::steady_clock;
template<class F> void measure(const char* name,std::size_t count,F function){
    std::size_t checksum=0;auto begin=Clock::now();
    for(std::size_t i=0;i<count;++i)checksum+=function(i);
    auto elapsed=std::chrono::duration<double,std::micro>(Clock::now()-begin).count();
    std::cout<<"{\"scenario\":\""<<name<<"\",\"iterations\":"<<count<<",\"us_per_op\":"<<elapsed/count<<",\"checksum\":"<<checksum<<"}\n";
}
int main(int argc,char** argv){try{
    if(argc>1 && std::string_view(argv[1])!="all")throw std::invalid_argument("usage: benchmark [all] [iterations]");
    auto count=argc>2?std::stoull(argv[2]):20000;if(!count || count>10000000)throw std::invalid_argument("iteration limit");
    auto connection=ConnectionRef::create();EntitySchema schema;
    for(unsigned i=0;i<32;++i)schema.attributes.emplace("field"+std::to_string(i),AttributeDefinition{AttributeType::integer,std::int64_t(0)});
    EntityAttributes model(schema,connection.key());
    measure("carrier_stage_publish_32",count,[&](auto i){auto mutation=model.stage({{"field0",std::int64_t(i)}},{Caller::server,"benchmark"});if(!model.publish(mutation))throw std::runtime_error("stale carrier");return model.revision();});
    AttributeUpdate before{0,1,true,{{"pose",AttributeStruct{{"x",0.0},{"y",0.0},{"label",std::string(65536,'p')}}}}};auto after=before;after.revision=2;
    std::get<AttributeStruct>(after.values.at("pose")).at("x")=1.0;
    measure("sparse_field_encode_64k",count,[&](auto){auto delta=attribute_delta(before,after);return encode_attributes(delta).size();});
    auto delta=attribute_delta(before,after);
    measure("sparse_field_replica_64k",count,[&](auto){AttributeReplica replica;if(!replica.apply(before)||!replica.apply(delta))throw std::runtime_error("replica");return replica.image_bytes();});
    AttributeArray items;for(unsigned i=0;i<32;++i)items.emplace_back(std::int64_t(i));
    AttributeUpdate inventory{0,1,true,{{"items",items}}},changed=inventory;changed.revision=2;std::get<AttributeArray>(changed.values.at("items")).emplace_back(std::int64_t(32));
    measure("container_append_delta_32",count,[&](auto){return encode_attributes(attribute_delta(inventory,changed)).size();});
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}

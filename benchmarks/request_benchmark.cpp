#include "fsnet/client_gateway.hpp"
#include "fsnet/entity_message.hpp"
#include "fsnet/entity_record.hpp"
#include <algorithm>
#include <chrono>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>
using Clock=std::chrono::steady_clock;
using namespace std::chrono_literals;
// Framework fixture only: plaintext loopback and a fixed trusted principal.
// Measures two-hop RPC overhead, not a production authentication policy.
int main(int argc,char** argv){try{
#ifndef NDEBUG
    throw std::runtime_error("use a Release build");
#endif
    const std::string mode=argc>1?argv[1]:"gateway";
    const auto count=argc>2?std::stoull(argv[2]):10000ULL;
    const auto bytes=argc>3?std::stoull(argv[3]):128ULL;
    const auto concurrency=argc>4?std::stoull(argv[4]):32ULL;
    if((mode!="direct"&&mode!="gateway")||!count||count>1000000||bytes>262144||!concurrency||concurrency>64)throw std::invalid_argument("direct|gateway count[1..1M] bytes[0..256K] concurrency[1..64]");
    fsnet::Node owner("benchmark"),front("benchmark"),back("benchmark"),client("benchmark");
    auto database=fsnet::memory_database();
    auto expires=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count()+3600000;
    fsnet::EntityCatalog catalog(*database);
    auto id=catalog.create("benchmark","echo",{"Bench",{},"","owner"});
    auto saved=catalog.find(id);auto record=saved->record;
    record.owner="owner/benchmark";record.expires_ms=expires;record.fence=1;
    if(!database->compare_exchange(fsnet::entity_record_key(id),saved->version,fsnet::encode_entity_record(record)))throw std::runtime_error("directory fixture failed");
    auto host=std::make_shared<fsnet::EntityScriptHost>(std::make_shared<fsnet::NodeScriptHost>(owner),database,"owner/benchmark");
    auto base=[&](std::uint64_t,const std::string&)->std::optional<fsnet::CallIdentity>{return fsnet::CallIdentity{mode=="gateway"?fsnet::Caller::server:fsnet::Caller::client,mode=="gateway"?"gateway":"alice"};};
    owner.identify(base);
    owner.bind_rpc(fsnet::entity_call_route,{true,false,false},[](const fsnet::CallIdentity& who,std::string data,fsnet::Reply reply){
        if(who.caller!=fsnet::Caller::client||who.subject!="alice")throw std::runtime_error("incorrect benchmark delegation");
        reply({fsnet::Status::ok,std::move(data)});
    });
    auto owner_port=owner.listen("127.0.0.1",0);
    std::unique_ptr<fsnet::GatewayDelegation> delegation;
    std::unique_ptr<fsnet::ClientGateway> gateway;
    auto port=owner_port;
    if(mode=="gateway"){
        delegation=std::make_unique<fsnet::GatewayDelegation>(owner,base,[](const auto& who,const auto&){return who.caller==fsnet::Caller::server&&who.subject=="gateway";},host);
        front.identify([](std::uint64_t,const std::string&){return std::optional(fsnet::CallIdentity{fsnet::Caller::client,"alice"});});
        gateway=std::make_unique<fsnet::ClientGateway>(front,back,database,std::vector<fsnet::EntityEndpoint>{{"owner","127.0.0.1",owner_port,{"Bench"}}});
        gateway->bind_entity("Bench",{"echo"});port=front.listen("127.0.0.1",0);
    }
    auto peer=client.connect("127.0.0.1",port);
    auto payload=fsnet::entity_call_message(id,"echo",std::string(bytes,'x'));
    auto pump=[&]{owner.poll();front.poll();back.poll();client.poll();host->poll();if(delegation)delegation->poll();if(gateway)gateway->poll();};
    // Establish the transport/delegation/route before measuring steady traffic.
    bool warmed=false;client.call(peer,fsnet::entity_call_route,payload,5s,[&](auto result){if(result.status!=fsnet::Status::ok||result.data!=payload)throw std::runtime_error("warmup failed");warmed=true;});
    auto deadline=Clock::now()+10s;while(!warmed){pump();if(Clock::now()>deadline)throw std::runtime_error("warmup deadline");}
    std::size_t sent=0,completed=0;std::vector<double> latency;latency.reserve(count);
    std::function<void()> submit;
    submit=[&]{if(sent>=count)return;++sent;auto begin=Clock::now();client.call(peer,fsnet::entity_call_route,payload,5s,[&,begin](auto result){
        if(result.status!=fsnet::Status::ok||result.data!=payload)throw std::runtime_error("RPC failed");
        latency.push_back(std::chrono::duration<double,std::micro>(Clock::now()-begin).count());++completed;submit();
    });};
    auto begin=Clock::now();for(std::size_t i=0;i<std::min(count,concurrency);++i)submit();
    while(completed<count){pump();if(Clock::now()-begin>60s)throw std::runtime_error("benchmark deadline");}
    auto seconds=std::chrono::duration<double>(Clock::now()-begin).count();std::sort(latency.begin(),latency.end());
    std::cout<<"{\"scenario\":\""<<mode<<"\",\"requests\":"<<count<<",\"payload_bytes\":"<<bytes<<",\"concurrency\":"<<concurrency<<",\"seconds\":"<<seconds<<",\"requests_per_second\":"<<count/seconds<<",\"p50_us\":"<<latency[count/2]<<",\"p99_us\":"<<latency[(count-1)*99/100]<<"}\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}

#include "../common/config.hpp"
#include "fsnet/client_gateway.hpp"
#include "fsnet/entity_transfer.hpp"
#include "fsnet/attribute_replication.hpp"
#include "fsnet/lua_engine.hpp"
#include <algorithm>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>
#include <thread>
#include <unordered_map>
using namespace std::chrono_literals;
namespace {
volatile std::sig_atomic_t running=1;
void signal_stop(int){running=0;}
std::string env(const char* key,const std::string& fallback={}){auto value=std::getenv(key);return value?value:fallback;}
std::vector<std::vector<std::string>> rows(const std::string& path,std::size_t columns){
    std::ifstream file(path);if(!file)throw std::runtime_error("manifest cannot be opened: "+path);
    std::vector<std::vector<std::string>> result;std::string line;
    while(std::getline(file,line)){if(line.empty()||line[0]=='#')continue;std::istringstream in(line);std::vector<std::string> row;std::string value;while(in>>value)row.push_back(value);if(row.size()!=columns||result.size()>=4096)throw std::runtime_error("invalid manifest row");result.push_back(std::move(row));}return result;
}
std::unordered_map<std::string,std::string> identities(const std::string& path){std::unordered_map<std::string,std::string> result;for(auto& r:rows(path,2)){
    if(r[0].size()!=71||!r[0].starts_with("sha256:")||!std::all_of(r[0].begin()+7,r[0].end(),[](char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');})||r[1].empty()||r[1].size()>256||!result.emplace(r[0],r[1]).second)throw std::runtime_error("invalid identity manifest");}if(result.empty())throw std::runtime_error("empty identity manifest");return result;}
std::vector<std::string> split(const std::string& text){std::vector<std::string> result;std::istringstream in(text);std::string item;while(std::getline(in,item,',')){if(item.empty())throw std::runtime_error("empty list item");result.push_back(item);}return result;}
unsigned port(const std::string& text){std::size_t end=0;auto n=std::stoul(text,&end);if(end!=text.size()||n>65535)throw std::runtime_error("invalid port");return static_cast<unsigned>(n);}
std::string script(const std::string& path){std::ifstream file(path,std::ios::binary|std::ios::ate);if(!file||file.tellg()<0||file.tellg()>1024*1024)throw std::runtime_error("script missing or exceeds 1 MiB");std::string out(static_cast<std::size_t>(file.tellg()),'\0');file.seekg(0);if(!file.read(out.data(),static_cast<std::streamsize>(out.size())))throw std::runtime_error("script read failed");return out;}
void metrics(const std::string& mode,bool ready,const fsnet::TransportStats& transport,const std::string& fields){
    auto path=env("FSNET_METRICS_FILE");if(path.empty())return;
    auto temporary=path+".tmp";std::ofstream file(temporary,std::ios::trunc);if(!file)throw std::runtime_error("metrics file cannot be written");
    file<<"{\"mode\":\""<<mode<<"\",\"ready\":"<<(ready?"true":"false")<<",\"connections\":"<<transport.connections<<",\"transport_pending\":"<<transport.pending<<",\"transport_inflight\":"<<transport.inflight<<",\"transport_queued_bytes\":"<<transport.queued_bytes<<fields<<"}\n";
    file.close();if(!file)throw std::runtime_error("metrics write failed");std::filesystem::rename(temporary,path);
}
fsnet::TlsConfig tls(){auto result=tls_from_environment();if(result.ca_file.empty()||result.certificate_file.empty()||result.private_key_file.empty())throw std::runtime_error("service launcher requires mutual TLS CA/certificate/key");return result;}
std::string incarnation(const std::string& node){if(node.empty()||node.size()>127||node.find_first_of("/\0",0,2)!=std::string::npos)throw std::runtime_error("invalid stable node ID");std::random_device random;return node+'/'+std::to_string(random())+'-'+std::to_string(random())+'-'+std::to_string(random());}
}
int main(int argc,char** argv){try{
    if(argc<2)throw std::runtime_error("usage: fsnet-service owner SCRIPT PORT NODE GATEWAY_PINS | gateway PORT ENDPOINTS CLIENT_IDENTITIES ROUTES");
    auto storage_uri=env("FSNET_STORAGE_URI");if(storage_uri.empty()||storage_uri=="memory:")throw std::runtime_error("service requires an explicit persistent FSNET_STORAGE_URI");
    auto database=fsnet::open_storage(storage_uri);auto security=tls();auto token=env("FSNET_CLUSTER_TOKEN");
    std::signal(SIGINT,signal_stop);std::signal(SIGTERM,signal_stop);
    std::string mode=argv[1];auto next_metrics=std::chrono::steady_clock::now();
    if(mode=="owner"){
        if(argc!=6)throw std::runtime_error("usage: fsnet-service owner SCRIPT PORT NODE GATEWAY_PINS");
        auto gateway_pins=identities(argv[5]),pins=gateway_pins;
        auto endpoint_path=env("FSNET_ENTITY_ENDPOINTS"),owner_path=env("FSNET_OWNER_PINS");
        if(endpoint_path.empty()!=owner_path.empty()) throw std::runtime_error("entity transfer requires both FSNET_ENTITY_ENDPOINTS and FSNET_OWNER_PINS");
        std::unordered_map<std::string,std::string> owner_pins;
        std::vector<fsnet::EntityEndpoint> transfer_endpoints;
        if(!endpoint_path.empty()) {
            owner_pins=identities(owner_path);
            for(const auto& [cert,subject]:owner_pins) {
                if(gateway_pins.contains(cert)) throw std::runtime_error("owner and gateway certificate pins must be separate");
                pins.emplace(cert,subject);
            }
            for(auto& row:rows(endpoint_path,5)) transfer_endpoints.push_back({row[0],row[1],static_cast<std::uint16_t>(port(row[2])),split(row[3]),row[4]});
        }
        fsnet::Node node(token,{},security);
        node.authorize([pins](std::uint64_t,const std::string& cert,const std::string&){return pins.contains(cert);});
        auto base=[pins](std::uint64_t,const std::string& cert)->std::optional<fsnet::CallIdentity>{auto it=pins.find(cert);if(it==pins.end())return {};return fsnet::CallIdentity{fsnet::Caller::server,it->second};};
        fsnet::EntityRuntimeLimits limits;
        auto lease=std::stoll(env("FSNET_ENTITY_LEASE_MS","60000"));if(lease<500||lease>86400000)throw std::runtime_error("lease out of range");limits.lease=std::chrono::milliseconds(lease);limits.request_timeout=std::min(30000ms,limits.lease/2);
        auto host=std::make_shared<fsnet::EntityScriptHost>(std::make_shared<fsnet::NodeScriptHost>(node),database,incarnation(argv[4]),limits);
        fsnet::AttributeReplication replication(node,host);
        fsnet::GatewayDelegation delegation(node,base,[gateway_pins](const fsnet::CallIdentity& who,const std::string& cert){auto it=gateway_pins.find(cert);return it!=gateway_pins.end()&&it->second==who.subject&&who.caller==fsnet::Caller::server;},host);
        std::unique_ptr<fsnet::EntityTransfer> transfer;
        if(!transfer_endpoints.empty()) transfer=std::make_unique<fsnet::EntityTransfer>(node,host,database,std::move(transfer_endpoints),
            [owner_pins,&node](const fsnet::CallIdentity& who) { auto it=owner_pins.find(node.peer_identity(who.peer)); return who.caller==fsnet::Caller::server && it!=owner_pins.end() && it->second==who.subject; });
        fsnet::LuaEngine engine(host);engine.load(script(argv[2]),argv[2]);
        auto listen_port=node.listen(bind_from_environment(),static_cast<std::uint16_t>(port(argv[3])));std::cout<<"listening "<<listen_port<<std::endl;
        auto pump=[&]{node.poll();host->poll();engine.poll();delegation.poll();replication.poll();if(transfer)transfer->poll();};
        auto report=[&](bool ready){auto s=host->stats();metrics(mode,ready,node.stats(),",\"entities\":"+std::to_string(s.entities)+",\"pending\":"+std::to_string(s.pending)+",\"queued_bytes\":"+std::to_string(s.queued_bytes)+",\"resident_bytes\":"+std::to_string(s.state_bytes)+",\"transient_bytes\":"+std::to_string(s.transient_bytes)+",\"committed\":"+std::to_string(s.committed)+",\"rejected\":"+std::to_string(s.rejected)+",\"failed\":"+std::to_string(s.failed)+",\"publication_failures\":"+std::to_string(s.publication_failures)+",\"lua_active\":"+std::to_string(engine.active())+",\"lua_bytes\":"+std::to_string(engine.memory_used()));};
        while(running){pump();if(std::chrono::steady_clock::now()>=next_metrics){report(true);next_metrics=std::chrono::steady_clock::now()+100ms;}std::this_thread::sleep_for(1ms);}
        node.set_ready(false);report(false);auto deadline=std::chrono::steady_clock::now()+35s;bool drained=false;fsnet::Status status=fsnet::Status::overloaded;
        while(!drained&&std::chrono::steady_clock::now()<deadline){
            auto complete=std::make_shared<std::optional<fsnet::Status>>();
            host->drain([complete](auto result){*complete=result.status;});
            while(!*complete&&std::chrono::steady_clock::now()<deadline){pump();std::this_thread::sleep_for(1ms);}
            if(*complete)status=**complete;
            drained=complete->has_value()&&status==fsnet::Status::ok;
            if(!drained){pump();std::this_thread::sleep_for(1ms);}
        }
        auto flush=std::chrono::steady_clock::now()+250ms;while(std::chrono::steady_clock::now()<flush){pump();std::this_thread::sleep_for(1ms);}
        report(false);engine.stop();if(transfer)transfer->stop();replication.stop();delegation.stop();host->stop();node.stop();if(!drained)throw std::runtime_error("owner drain deadline exceeded");
    }else if(mode=="gateway"){
        if(argc!=6)throw std::runtime_error("usage: fsnet-service gateway PORT ENDPOINTS CLIENT_IDENTITIES ROUTES");
        std::vector<fsnet::EntityEndpoint> endpoints;for(auto& row:rows(argv[3],5))endpoints.push_back({row[0],row[1],static_cast<std::uint16_t>(port(row[2])),split(row[3]),row[4]});
        auto clients=identities(argv[4]);fsnet::Node front(token,{},security),back(token,{},security);
        front.authorize([clients](std::uint64_t,const std::string& cert,const std::string&){return clients.contains(cert);});
        front.identify([clients](std::uint64_t,const std::string& cert)->std::optional<fsnet::CallIdentity>{auto it=clients.find(cert);if(it==clients.end())return {};return fsnet::CallIdentity{fsnet::Caller::client,it->second};});
        fsnet::ClientGateway gateway(front,back,database,std::move(endpoints));
        for(auto& row:rows(argv[5],3)){if(row[0]=="entity")gateway.bind_entity(row[1],split(row[2]));else if(row[0]=="account")gateway.bind_account(row[1],row[2]);else throw std::runtime_error("unknown route kind");}
        auto listen_port=front.listen(bind_from_environment(),static_cast<std::uint16_t>(port(argv[2])));std::cout<<"listening "<<listen_port<<std::endl;
        auto pump=[&]{front.poll();back.poll();gateway.poll();};
        auto report=[&](bool ready){auto s=gateway.stats();metrics(mode,ready,front.stats(),",\"clients\":"+std::to_string(s.clients)+",\"links\":"+std::to_string(s.links)+",\"subscriptions\":"+std::to_string(s.subscriptions)+",\"pending\":"+std::to_string(s.pending)+",\"queued_bytes\":"+std::to_string(s.queued_bytes)+",\"migrating\":"+std::to_string(s.migrating)+",\"forwarded\":"+std::to_string(s.forwarded)+",\"failures\":"+std::to_string(s.failures)+",\"migrations\":"+std::to_string(s.migrations)+",\"backend_pending\":"+std::to_string(back.pending()));};
        while(running){pump();if(std::chrono::steady_clock::now()>=next_metrics){report(true);next_metrics=std::chrono::steady_clock::now()+100ms;}std::this_thread::sleep_for(1ms);}
        front.set_ready(false);gateway.drain();report(false);auto deadline=std::chrono::steady_clock::now()+4s;
        while(gateway.stats().pending&&std::chrono::steady_clock::now()<deadline){pump();std::this_thread::sleep_for(1ms);}gateway.stop();
        auto flush=std::chrono::steady_clock::now()+250ms;while(std::chrono::steady_clock::now()<flush){front.poll();back.poll();std::this_thread::sleep_for(1ms);}report(false);back.stop();front.stop();
    }else throw std::runtime_error("unknown service mode");
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}

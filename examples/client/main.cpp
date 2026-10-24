#include "../common/config.hpp"
#include "fsnet/client.hpp"
#include "fsnet/lua_engine.hpp"
#include <csignal>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <thread>
namespace { volatile std::sig_atomic_t running=1;void stop(int){running=0;} }
int main(int argc,char** argv){try{
    std::setvbuf(stdout,nullptr,_IOLBF,0);
    if(argc!=4)throw std::invalid_argument("usage: fsnet-client script.lua address port");
    auto* token=std::getenv("FSNET_CLUSTER_TOKEN");if(!token)throw std::runtime_error("set FSNET_CLUSTER_TOKEN");
    std::size_t end=0;auto port=std::stoul(argv[3],&end);if(end!=std::string(argv[3]).size()||!port||port>65535)throw std::invalid_argument("invalid port");
    std::ifstream file(argv[1],std::ios::binary|std::ios::ate);
    if(!file||file.tellg()<0||file.tellg()>1024*1024)throw std::runtime_error("script missing or exceeds 1 MiB");
    std::string source(static_cast<std::size_t>(file.tellg()),'\0');file.seekg(0);
    if(!file.read(source.data(),static_cast<std::streamsize>(source.size())))throw std::runtime_error("cannot read script");
    fsnet::ClientOptions options;options.token=token;options.tls=tls_from_environment();
    fsnet::Client client(std::move(options));bool disconnected=false;
    client.on_event([&](const auto& event){if(event.kind==fsnet::ClientEvent::Kind::disconnected&&event.status!=fsnet::Status::ok)disconnected=true;});
    client.connect({argv[2],static_cast<std::uint16_t>(port),{}});
    fsnet::LuaEngine scripts(client.script_host());scripts.load(source,argv[1]);
    std::signal(SIGINT,stop);std::signal(SIGTERM,stop);
    while(running && scripts.active()){
        client.poll();scripts.poll();if(disconnected)break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    auto error=scripts.last_error();if(error.empty())error=client.last_error();
    scripts.stop();client.disconnect();
    if(!error.empty())throw std::runtime_error(error);
    if(disconnected)throw std::runtime_error("client connection closed");
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}

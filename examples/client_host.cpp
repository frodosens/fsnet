#include "config.hpp"
#include "fsnet/lua_engine.hpp"
#include "fsnet/client_script_host.hpp"
#include <csignal>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <thread>
namespace { volatile std::sig_atomic_t running=1; void stop(int) { running=0; } }
int main(int argc,char** argv) {
    try {
        std::setvbuf(stdout,nullptr,_IOLBF,0);
        if(argc!=4) throw std::invalid_argument("usage: fsnet-client script.lua address port");
        auto* token=std::getenv("FSNET_CLUSTER_TOKEN");
        if(!token) throw std::runtime_error("set FSNET_CLUSTER_TOKEN");
        auto port=std::stoul(argv[3]); if(!port || port>65535) throw std::invalid_argument("invalid port");
        std::ifstream file(argv[1],std::ios::binary|std::ios::ate);
        if(!file || file.tellg()<0 || file.tellg()>1024*1024) throw std::runtime_error("script missing or exceeds 1 MiB");
        std::string source(static_cast<std::size_t>(file.tellg()),'\0'); file.seekg(0);
        if(!file.read(source.data(),static_cast<std::streamsize>(source.size()))) throw std::runtime_error("cannot read script");
        fsnet::Node node(token,{},tls_from_environment());
        auto peer=node.connect(argv[2],static_cast<std::uint16_t>(port));
        auto host=std::make_shared<fsnet::ClientScriptHost>(node,peer);
        fsnet::LuaEngine engine(host); engine.load(source,argv[1]);
        std::signal(SIGINT,stop); std::signal(SIGTERM,stop);
        while(running && engine.active()) {
            node.poll(); host->poll(); engine.poll(); std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        auto error=engine.last_error(); engine.stop(); host->stop(); node.stop();
        if(!error.empty()) throw std::runtime_error(error);
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}

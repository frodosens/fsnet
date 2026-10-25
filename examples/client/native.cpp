#include "../common/config.hpp"
#include "fsnet/client.hpp"
#include <iostream>
#include <thread>
using namespace std::chrono_literals;
// Native engine adapter example: session, presentation/control events, confirmed
// snapshots and RPC. Movement and prediction rules live in scene/client.lua.
int main(int argc,char** argv){try{
    if(argc<4||argc>5)throw std::invalid_argument("usage: fsnet-native-client address port entry-id [display-name]");
    auto* token=std::getenv("FSNET_CLUSTER_TOKEN");if(!token)throw std::runtime_error("set FSNET_CLUSTER_TOKEN");
    std::size_t end=0;auto port=std::stoul(argv[2],&end);if(end!=std::string(argv[2]).size()||!port||port>65535)throw std::invalid_argument("invalid port");
    fsnet::ClientOptions options;options.token=token;options.tls=tls_from_environment();fsnet::Client client(std::move(options));
    fsnet::EntityId entry_id(argv[3]);
    bool logged_in=false,requested=false,left=false,logout_complete=false,done=false;std::string error;
    auto check=[&](fsnet::Result result){if(result.status!=fsnet::Status::ok)error="RPC failed: "+std::to_string(static_cast<unsigned>(result.status));};
    client.on_event([&](const fsnet::ClientEvent& event){
        using Kind=fsnet::ClientEvent::Kind;
        if(event.kind==Kind::connected)client.call(entry_id,"login",argc==5?argv[4]:"Alice",[&](auto result){check(result);logged_in=result.status==fsnet::Status::ok;});
        if(event.kind==Kind::enter && event.automatic)std::cout<<(event.owned.value_or(false)?"LocalPlayer ":"RemotePlayer ")<<event.key.id<<'\n';
        if(event.kind==Kind::control)std::cout<<(event.controlled?"camera follow ":"camera release ")<<event.key.id<<'\n';
        if(event.kind==Kind::leave && event.owned==true){
            if(event.controlled)std::cout<<"camera release "<<event.key.id<<'\n';
            left=true;done=left && logout_complete;
        }
        if(event.kind==Kind::disconnected && !done)error="connection closed";
    });
    client.connect({argv[1],static_cast<std::uint16_t>(port),{}});
    auto deadline=std::chrono::steady_clock::now()+10s;
    while(!done && error.empty() && std::chrono::steady_clock::now()<deadline){
        client.poll();
        auto player=client.controlled();
        if(logged_in && player && !requested){
            requested=true;
            std::cout<<"confirmed name "<<std::get<std::string>(player->confirmed->values.at("name"))<<'\n';
            client.call(player->key,"describe","",[&](auto result){
                check(result);if(!error.empty())return;
                std::cout<<"describe "<<result.data<<'\n';
                client.call(entry_id,"logout","",[&](auto result){check(result);logout_complete=result.status==fsnet::Status::ok;done=left && logout_complete;});
            });
        }
        if(!client.last_error().empty())error=client.last_error();
        std::this_thread::sleep_for(1ms);
    }
    client.disconnect();
    if(!error.empty())throw std::runtime_error(error);
    if(!done)throw std::runtime_error("native example deadline");
    std::cout<<"native complete\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}

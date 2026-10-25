#include "fsnet/client_gateway.hpp"
#include <iostream>
#include <thread>
using namespace fsnet;using namespace std::chrono_literals;
#define CHECK(c) do{if(!(c))throw std::runtime_error(std::string(__FILE__)+":"+std::to_string(__LINE__)+" " #c);}while(false)
int main(){try{
    auto connection=ConnectionRef::create();CHECK(connection.matches_key(connection.key()));
    CHECK(!connection.matches_key("connection/"+connection.id+"/01"));
    CHECK(!connection.matches_key("connection/"+connection.id+"/2"));
    CHECK(!connection.matches_key("connection/"+std::string(32,'z')+"/1"));
    unsigned checks=0;CallIdentity permission{Caller::client,"fixture",0,connection,[&]{++checks;return true;}};
    CHECK((RpcPolicy{true,false,true}.allows(permission,connection.key())));CHECK(checks==1);
    Node server("test"),a("test"),b("test"),front("test"),back("test");
    auto db=memory_database();auto host=std::make_shared<EntityScriptHost>(std::make_shared<NodeScriptHost>(server),db,"server/boot");
    auto base=[](std::uint64_t,const std::string&)->std::optional<CallIdentity>{return CallIdentity{Caller::server,"fixture"};};
    GatewayDelegation delegation(server,base,[](const CallIdentity& who,const std::string&){return who.caller==Caller::server&&who.subject=="fixture";},host);
    std::vector<CallIdentity> captured;
    server.bind_rpc("who",{true,true,false},[&](const CallIdentity& who,std::string,Reply reply){captured.push_back(who);reply({Status::ok,who.connection.key()});});
    auto port=server.listen("127.0.0.1",0);
    auto peer_a=a.connect("127.0.0.1",port),peer_b=b.connect("127.0.0.1",port);
    std::uint64_t frontend=0;
    front.identify([&](std::uint64_t peer,const std::string&)->std::optional<CallIdentity>{frontend=peer;return CallIdentity{Caller::client,"same-account"};});
    ClientGateway gateway(front,back,db,{{"server","127.0.0.1",port,{"Actor"}}});gateway.bind_account("who","Actor");
    auto gateway_port=front.listen("127.0.0.1",0);
    auto pump=[&]{server.poll();a.poll();b.poll();front.poll();back.poll();host->poll();delegation.poll();gateway.poll();};
    auto until=[&](auto done){auto end=std::chrono::steady_clock::now()+3s;while(!done()){pump();CHECK(std::chrono::steady_clock::now()<end);std::this_thread::sleep_for(1ms);}};
    auto call=[&](Node& node,std::uint64_t peer){std::optional<Result> result;node.call(peer,"who","",1s,[&](Result value){result=std::move(value);});until([&]{return result.has_value();});CHECK(result->status==Status::ok);return ConnectionRef::from_key(result->data);};
    auto first=call(a,peer_a),second=call(b,peer_b);CHECK(first!=second && first.key()==ConnectionRef::from_key(first.key()).key());
    auto identity=captured.front();CHECK(identity.live());RpcPolicy owner{false,true,true};CHECK(owner.allows(identity,first.key()));CHECK(!owner.allows(captured.back(),first.key()));
    server.close(identity.peer);CHECK(!identity.live() && !owner.allows(identity,first.key()));
    auto replacement=call(a,a.connect("127.0.0.1",port));CHECK(replacement!=first);
    auto proxied=call(a,a.connect("127.0.0.1",gateway_port));CHECK(front.identity(frontend)->connection==proxied);
    CHECK(captured.back().caller==Caller::client && captured.back().subject=="same-account");
    auto old_proxy=captured.back();front.close(frontend);until([&]{return !old_proxy.live();});
    for(auto invalid:{"","connection/x/1","connection/00000000000000000000000000000000/0","connection/00000000000000000000000000000000/01"}) {
        bool denied=false;try{ConnectionRef::from_key(invalid);}catch(...){denied=true;}CHECK(denied);
    }
    gateway.stop();delegation.stop();host->stop();server.stop();a.stop();b.stop();front.stop();back.stop();
    std::cout<<"PASS connection identity: unique sessions, serialization, trusted Gateway forwarding, disconnect and reconnect fencing\n";
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}

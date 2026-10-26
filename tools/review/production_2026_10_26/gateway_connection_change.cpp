#include "fsnet/client_gateway.hpp"
#include "fsnet/attribute_replication.hpp"
#include "fsnet/entity_message.hpp"
#include "fsnet/replication_wire.hpp"
#include <iostream>
#include <thread>
using namespace fsnet;using namespace std::chrono_literals;
#define CHECK(x) do{if(!(x))throw std::runtime_error(#x);}while(false)
int main(){try{
 Node owner("review"),front("review"),back("review"),client("review");auto db=memory_database();
 auto host=std::make_shared<EntityScriptHost>(std::make_shared<NodeScriptHost>(owner),db,"owner/boot");
 EntitySchema schema;schema.attributes.emplace("secret",AttributeDefinition{AttributeType::integer,std::int64_t(1),Access::read_write,Access::read,Visibility::owner,true});host->define_schema("Actor",schema);
 host->bind_stateful_context("Actor.bump","Actor",encode_attributes({0,0,true,{{"secret",std::int64_t(1)}}}),{false,true,false},[](const auto&,std::string input,Reply reply){auto entity=parse_entity_message_view(input);auto state=parse_state_message_view(entity->payload);auto image=decode_attributes(state->state);++std::get<std::int64_t>(image.values.at("secret"));++image.revision;reply({Status::ok,state_message(encode_attributes(image),"")});});
 auto base=[](auto,const auto&)->std::optional<CallIdentity>{return CallIdentity{Caller::server,"backend"};};
 GatewayDelegation delegation(owner,base,[](const auto& who,const auto&){return who.caller==Caller::server&&who.subject=="backend";},host);
 AttributeReplication replication(owner,host);auto port=owner.listen("127.0.0.1",0);
 auto logical=ConnectionRef::create();auto original=logical;std::uint64_t frontend=0;
 front.identify([&](auto peer,const auto&)->std::optional<CallIdentity>{frontend=peer;return CallIdentity{Caller::client,"same-account",peer,logical};});
 GatewayLimits limits;limits.refresh=5ms;ClientGateway gateway(front,back,db,{{"owner","127.0.0.1",port,{"Actor"}}},limits);
 auto gate=front.listen("127.0.0.1",0);auto peer=client.connect("127.0.0.1",gate);
 int notices=0;std::int64_t private_value=0;
 client.on_notification("system.attributes.update",[&](auto,const auto&,std::string data){auto packet=decode_replication_packet(data);if(packet.update && packet.update->values.contains("secret")){++notices;private_value=std::get<std::int64_t>(packet.update->values.at("secret"));}});
 auto pump=[&]{owner.poll();front.poll();back.poll();client.poll();host->poll();replication.poll();delegation.poll();gateway.poll();};
 auto until=[&](auto done){auto end=std::chrono::steady_clock::now()+4s;while(!done()){pump();CHECK(std::chrono::steady_clock::now()<end);std::this_thread::sleep_for(1ms);}};
 auto wait=[&](auto begin){std::optional<Result> result;begin([&](Result r){result=std::move(r);});until([&]{return bool(result);});return *result;};
 auto created=wait([&](auto reply){host->create_entity("Actor",{},{"review","one",original,"owner"},std::move(reply));});CHECK(created.status==Status::ok);auto id=EntityId(created.data);
 auto watch=wait([&](auto reply){client.call(peer,"system.attributes.watch",encode_entity_key("Actor",id.str()),1s,std::move(reply));});CHECK(watch.status==Status::ok);
 logical=ConnectionRef::create();CHECK(logical!=original);auto end=std::chrono::steady_clock::now()+50ms;while(std::chrono::steady_clock::now()<end){pump();std::this_thread::sleep_for(1ms);}
 auto bump=wait([&](auto reply){host->call_entity("@",entity_call_message(id,"bump",""),1s,std::move(reply));});CHECK(bump.status==Status::ok);
 end=std::chrono::steady_clock::now()+100ms;while(std::chrono::steady_clock::now()<end){pump();std::this_thread::sleep_for(1ms);}
 std::cout<<"logical_changed="<<(front.identity(frontend)->connection!=original)<<" frontend_connected="<<client.connected(peer)<<" gateway_sessions="<<gateway.stats().clients<<" private_notifications="<<notices<<" private_value="<<private_value<<"\n";
 gateway.stop();replication.stop();delegation.stop();host->stop();owner.stop();front.stop();back.stop();client.stop();
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}

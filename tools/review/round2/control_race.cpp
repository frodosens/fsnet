#include "fsnet/client_gateway.hpp"
#include "fsnet/attribute_replication.hpp"
#include "fsnet/entity_record.hpp"
#include "fsnet/replication_wire.hpp"
#include <iostream>
#include <thread>
using namespace std::chrono_literals;
#define CHECK(x) do { if(!(x)) throw std::runtime_error(#x); } while(false)
int main(){try {
 auto db=fsnet::memory_database();
 for(auto type:{"Actor","Pet"}) { fsnet::EntityRecord rec;rec.owner="a/boot";rec.expires_ms=INT64_MAX;db->compare_exchange(fsnet::entity_record_key(type,"alice"),0,fsnet::encode_entity_record(rec)); }
 fsnet::Node owner("internal"),front("clients"),back("internal"),client("clients");
 front.identify([](auto,const auto&)->std::optional<fsnet::CallIdentity>{return fsnet::CallIdentity{fsnet::Caller::client,"alice"};});
 fsnet::AttributeUpdate image{0,1,true,{{"x",std::int32_t(7)}}};
 auto presence=[&](int kind,int flags,std::uint64_t id,std::string type){return std::string{char(kind),char(flags)}+fsnet::encode_replication_packet(id,type,"alice",fsnet::Status::ok,&image,30000);};
 std::uint64_t initial_peer=0,new_peer=0;fsnet::Reply actor_restore;bool actor_intent=false;unsigned reconciled=0;
 owner.bind("system.gateway.delegate",[](auto,auto reply){reply({fsnet::Status::ok,{}});});
 owner.bind_rpc("login",{true,false,false},[&](const auto& who,auto,auto reply){initial_peer=who.peer;owner.notify(who.peer,"system.entities.presence",presence(1,3,11,"Actor"));owner.notify(who.peer,"system.entities.presence",presence(1,1,12,"Pet"));reply({fsnet::Status::ok,{}});});
 owner.bind_rpc("system.gateway.restore",{true,false,false},[&](const auto& who,std::string data,auto reply){std::string_view in(data);auto [type,id]=fsnet::decode_entity_key(in);new_peer=who.peer;if(type=="Actor"){if(in.front()){actor_intent=true;actor_restore=std::move(reply);}else{++reconciled;reply({fsnet::Status::ok,presence(1,1,21,"Actor")});}}else reply({fsnet::Status::ok,presence(1,1,22,"Pet")});});
 owner.bind("system.attributes.unwatch",[](auto,auto reply){reply({fsnet::Status::ok,{}});});
 owner.bind("system.gateway.unpossess",[](auto,auto reply){reply({fsnet::Status::ok,{}});});
 auto port=owner.listen("127.0.0.1",0);
 fsnet::GatewayLimits limits;limits.refresh=5ms;limits.request_timeout=500ms;limits.migration_timeout=3s;
 fsnet::ClientGateway gateway(front,back,db,{{"a","127.0.0.1",port,{"Actor","Pet"}}},limits);gateway.bind_account("login","Actor");
 auto peer=client.connect("127.0.0.1",front.listen("127.0.0.1",0));std::uint64_t actor=0,pet=0;unsigned actor_leaves=0;bool pet_controlled=false;
 fsnet::AttributeClient attributes(client,peer,{},[&](const fsnet::EntityPresence& p){if(p.type=="Actor"){if(p.kind==fsnet::EntityPresence::Kind::enter)actor=p.subscription;if(p.kind==fsnet::EntityPresence::Kind::leave)++actor_leaves;}else {pet=p.subscription;pet_controlled=p.controlled;}});
 auto pump=[&]{owner.poll();front.poll();back.poll();client.poll();gateway.poll();attributes.poll();};
 auto until=[&](auto done){auto deadline=std::chrono::steady_clock::now()+3s;while(!done()){pump();CHECK(std::chrono::steady_clock::now()<deadline);std::this_thread::sleep_for(1ms);}};
 bool login=false;client.call(peer,"login","",1s,[&](auto r){CHECK(r.status==fsnet::Status::ok);login=true;});until([&]{return login&&actor&&pet;});
 owner.close(initial_peer);until([&]{return actor_restore&&gateway.stats().migrations==1;});CHECK(actor_intent);
 // Another entity becomes controlled while Actor's restore is still pending.
 owner.notify(new_peer,"system.entities.presence",presence(3,3,22,"Pet"));until([&]{return pet_controlled;});
 bool actor_before=attributes.get(actor)!=nullptr;
 actor_restore({fsnet::Status::ok,presence(1,3,21,"Actor")});until([&]{return actor_leaves>0||gateway.stats().migrations==2;});
 std::cout<<"restore_sent_control="<<actor_intent<<" pet_controlled="<<pet_controlled<<" actor_before_confirmation="<<actor_before<<" actor_after_confirmation="<<bool(attributes.get(actor))<<" actor_leaves="<<actor_leaves<<" gateway_subscriptions="<<gateway.stats().subscriptions<<"\n";
 return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}

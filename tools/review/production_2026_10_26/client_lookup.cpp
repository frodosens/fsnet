#include "fsnet/client.hpp"
#include "fsnet/attribute_replication.hpp"
#include "fsnet/entity_message.hpp"
#include <iostream>
#include <thread>
using namespace fsnet; using namespace std::chrono_literals;
int main(){try{
 std::uint64_t accepted=0;Node node("probe");node.authorize([&](auto peer,const auto&,const auto&){accepted=peer;return true;});node.identify([](auto,const auto&)->std::optional<CallIdentity>{return CallIdentity{Caller::client,"attacker"};});
 auto db=memory_database();auto host=std::make_shared<EntityScriptHost>(std::make_shared<NodeScriptHost>(node),db,"owner/boot");
 EntitySchema schema;schema.attributes.emplace("activated",AttributeDefinition{AttributeType::integer,std::int64_t(0)});
 host->define_schema("Actor",schema);auto initial=encode_attributes({0,0,true,{{"activated",std::int64_t(0)}}});
 auto unchanged=[](const CallIdentity&,std::string data,Reply reply){auto e=parse_entity_message(data);auto s=parse_state_message(e->payload);reply({Status::ok,state_message(s->state,"")});};
 host->bind_stateful_context("Actor.noop","Actor",initial,{false,true,false},unchanged);
 host->bind_stateful_context("Actor.$deactivate","Actor",initial,{false,true,false},unchanged);
 host->bind_stateful_context("Actor.$activate","Actor",initial,{false,true,false},[](const CallIdentity&,std::string data,Reply reply){auto e=parse_entity_message(data);auto s=parse_state_message(e->payload);auto image=decode_attributes(s->state);++std::get<std::int64_t>(image.values.at("activated"));++image.revision;reply({Status::ok,state_message(encode_attributes(image),"")});});
 AttributeReplication replication(node,host);ClientOptions options;options.token="probe";Client client(options);
 auto port=node.listen("127.0.0.1",0);client.connect({"127.0.0.1",port,{}});
 auto pump=[&]{node.poll();client.poll();host->poll();replication.poll();};
 auto until=[&](auto done){auto end=std::chrono::steady_clock::now()+5s;while(!done()){pump();if(std::chrono::steady_clock::now()>end)throw std::runtime_error("deadline");std::this_thread::sleep_for(1ms);}};
 until([&]{return client.state()==ClientState::connected;});
 std::optional<Result> create;host->create_entity("Actor",{},{"probe","actor",ConnectionRef::create(),"owner"},[&](auto r){create=std::move(r);});until([&]{return bool(create);});if(create->status!=Status::ok)throw std::runtime_error("create failed");auto id=EntityId(create->data);
 std::optional<Result> off;host->control_entity("Actor",id.str(),false,[&](auto r){off=std::move(r);});until([&]{return bool(off);});if(off->status!=Status::ok)throw std::runtime_error("deactivate failed");
 auto count=[&]{return std::get<std::int64_t>(decode_attributes(decode_entity_record(db->get(entity_record_key(id))->value).snapshot.state).values.at("activated"));};
 auto before=count();bool active_before=host->active("Actor",id.str());std::optional<Status> status;
 client.watch({"Actor",id.str()},[&](auto s,auto){status=s;});until([&]{return bool(status);});
 std::cout<<"watch_status="<<unsigned(*status)<<" activated_before="<<before<<" activated_after="<<count()<<" active_before="<<active_before<<" active_after="<<host->active("Actor",id.str())<<"\n";
 std::optional<Result> bound;host->bind_entity_owner("Actor",id.str(),node.identity(accepted)->connection,[&](auto r){bound=std::move(r);});until([&]{return bool(bound);});
 std::optional<Result> shown;host->entity_visibility(accepted,"Actor",id.str(),1,[&](auto r){shown=std::move(r);});until([&]{return bool(shown);});
 until([&]{return client.entity(id).has_value();});
 std::optional<Status> manual;client.watch({"Actor",id.str()},[&](auto status,auto){manual=status;});until([&]{return bool(manual);});
 auto by_id=client.entity(id),by_key=client.entity(EntityKey{"Actor",id.str()});
 std::cout<<"manual_status="<<unsigned(*manual)<<" entity_by_id_automatic="<<by_id->automatic<<" entity_by_key_automatic="<<by_key->automatic<<" by_id_owned_known="<<by_id->owned.has_value()<<" by_key_owned_known="<<by_key->owned.has_value()<<"\n";
 client.disconnect();replication.stop();host->stop();node.stop();
}catch(std::exception&e){std::cerr<<e.what()<<"\n";return 1;}}

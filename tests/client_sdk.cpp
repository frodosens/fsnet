#include "fsnet/client.hpp"
#include "fsnet/client_script_host.hpp"
#include "fsnet/entity_message.hpp"
#include <iostream>
#include <thread>
using namespace std::chrono_literals;
#define CHECK(x) do{if(!(x))throw std::runtime_error(std::string(__FILE__)+':'+std::to_string(__LINE__)+" " #x);}while(false)
int main(){try{
    fsnet::Node server("sdk-test");
    server.identify([](auto,const auto&){return std::optional(fsnet::CallIdentity{fsnet::Caller::client,"alice"});});
    auto host=std::make_shared<fsnet::EntityScriptHost>(std::make_shared<fsnet::NodeScriptHost>(server),fsnet::memory_database(),"owner/sdk");
    fsnet::EntitySchema schema;schema.legacy_owner_from_id=true;
    fsnet::AttributeDefinition text{fsnet::AttributeType::string,std::string("initial")};text.client=fsnet::Access::read_write;text.visibility=fsnet::Visibility::observers;
    schema.attributes.emplace("text",text);host->define_schema("Actor",schema);
    fsnet::EntityAttributes defaults(schema,"");auto initial=fsnet::encode_attributes({0,0,true,defaults.persistent()});
    host->bind_stateful_context("Actor.echo","Actor",initial,{true,false,true},[](const auto&,std::string data,fsnet::Reply reply){
        auto entity=fsnet::parse_entity_message_view(data);CHECK(entity);auto state=fsnet::parse_state_message_view(entity->payload);CHECK(state);
        reply({fsnet::Status::ok,fsnet::state_message(state->state,state->payload)});
    });
    host->bind_stateful_context("Actor.$write","Actor",initial,{true,false,true},[schema](const auto& who,std::string data,fsnet::Reply reply){
        auto entity=fsnet::parse_entity_message_view(data);CHECK(entity);auto state=fsnet::parse_state_message_view(entity->payload);CHECK(state);
        auto image=fsnet::decode_attributes(state->state);fsnet::EntityAttributes model(schema,"alice");model.restore_full(image.values,image.revision);
        auto mutation=fsnet::stage_attribute_proposal(model,fsnet::decode_attributes(state->payload),who);CHECK(model.publish(std::move(mutation)));
        reply({fsnet::Status::ok,fsnet::state_message(fsnet::encode_attributes({0,model.revision(),true,model.persistent()}),"")});
    });
    fsnet::AttributeReplication replication(server,host,[](const auto&,const auto&,const auto&){return true;});
    std::uint64_t physical=0;
    server.bind_rpc("login",{true,false,false},[&](const auto& who,std::string,fsnet::Reply reply){
        physical=who.peer;
        host->legacy_spawn_entity("Actor",who.subject,{{"text",std::string("ready")}},[&,who,reply=std::move(reply)](auto result) mutable {
            if(result.status!=fsnet::Status::ok)return reply(std::move(result));
            host->entity_visibility(who.peer,"Actor",who.subject,1,[&,who,reply=std::move(reply)](auto result) mutable {
                if(result.status!=fsnet::Status::ok)return reply(std::move(result));
                host->entity_visibility(who.peer,"Actor",who.subject,3,[who,reply=std::move(reply)](auto result) mutable {if(result.status==fsnet::Status::ok)result.data=who.subject;reply(std::move(result));});
            });
        });
    });
    server.bind_rpc("peer_id",{true,false,false},[](const auto& who,std::string,fsnet::Reply reply){reply({fsnet::Status::ok,std::to_string(who.peer)});});
    server.bind("wait",[](std::string,fsnet::Reply){});
    auto port=server.listen("127.0.0.1",0);
    fsnet::ClientOptions options;options.token="sdk-test";
    auto client=std::make_unique<fsnet::Client>(options);
    std::vector<fsnet::ClientEvent> events;
    client->on_event([&](const auto& event){events.push_back(event);});
    auto pump=[&]{server.poll();host->poll();replication.poll();if(client)client->poll();};
    unsigned stage=0;
    auto until=[&](auto done){++stage;auto end=std::chrono::steady_clock::now()+8s;while(!done()){pump();if(std::chrono::steady_clock::now()>end)throw std::runtime_error("SDK deadline stage="+std::to_string(stage)+" state="+std::to_string(client?static_cast<int>(client->state()):-1)+" entities="+std::to_string(client?client->stats().entities:0)+" error="+(client?client->last_error():"destroyed"));std::this_thread::sleep_for(1ms);}};
    std::optional<fsnet::Result> reply;
    client->request("login","",[&](auto r){reply=std::move(r);});CHECK(reply->status==fsnet::Status::disconnected);reply.reset();
    client->connect({"127.0.0.1",port,{}});
    // Requests may be queued during the transport handshake; identity still
    // comes from the server's trusted resolver, never from this display payload.
    client->request("login","bob",[&](auto r){reply=std::move(r);});
    until([&]{return reply && client->controlled();});CHECK(reply->status==fsnet::Status::ok && reply->data=="alice");
    auto owned=*client->controlled();CHECK(owned.key.id=="alice" && owned.owned==true);
    CHECK(events[0].kind==fsnet::ClientEvent::Kind::connecting && events[1].kind==fsnet::ClientEvent::Kind::connected);
    CHECK(std::get<std::string>(owned.confirmed->values.at("text"))=="ready");
    auto old=owned.confirmed;reply.reset();
    client->write(owned.handle,{{"text",std::string("changed")}},[&](auto r){reply=std::move(r);});
    until([&]{auto view=client->entity(owned.handle);return reply && view && std::get<std::string>(view->confirmed->values.at("text"))=="changed";});
    CHECK(reply->status==fsnet::Status::ok && std::get<std::string>(old->values.at("text"))=="ready");
    reply.reset();client->call(owned.key,"echo","",[&](auto r){reply=std::move(r);});CHECK(reply && reply->status==fsnet::Status::rejected);
    reply.reset();client->legacy_call(owned.key,"echo",std::string("a\0b",3),[&](auto r){reply=std::move(r);},{"stable-id",2s});
    until([&]{return reply.has_value();});CHECK(reply->data==std::string("a\0b",3));
    fsnet::ClientEntityHandle manual;
    client->watch({"Actor","bob"},[&](auto status,auto handle){CHECK(status==fsnet::Status::ok);manual=handle;CHECK(client->entity(handle));});
    until([&]{return bool(manual);});CHECK(!client->entity(manual)->owned.has_value());
    reply.reset();client->write(manual,{{"text",std::string("forbidden")}},[&](auto r){reply=std::move(r);});until([&]{return reply.has_value();});CHECK(reply->status==fsnet::Status::rejected);
    std::optional<fsnet::Result> shown;
    host->entity_visibility(physical,"Actor","bob",1,[&](auto r){shown=std::move(r);});
    until([&]{auto other=client->entity(fsnet::EntityKey{"Actor","bob"});return shown && other && other->automatic;});
    CHECK(client->entity(fsnet::EntityKey{"Actor","bob"})->owned==false);
    client->unwatch(manual);CHECK(!client->entity(manual));
    auto script_host=client->script_host();CHECK(script_host->pop_entity_event().has_value());
    script_host->bind_context("client_callback",{false,true,false},[](const auto& who,std::string data,fsnet::Reply reply){CHECK(who.caller==fsnet::Caller::server);reply({fsnet::Status::ok,std::move(data)});});
    std::optional<fsnet::Result> pushed;server.call(physical,"client_callback","server push",1s,[&](auto r){pushed=std::move(r);});
    until([&]{return pushed.has_value();});CHECK(pushed->status==fsnet::Status::ok && pushed->data=="server push");
    auto extra_peer=script_host->connect("127.0.0.1",port);CHECK(extra_peer);
    std::optional<fsnet::Result> extra_identity;
    script_host->call(extra_peer,"peer_id","",1s,[&](auto result){extra_identity=std::move(result);});until([&]{return extra_identity.has_value();});
    pushed.reset();server.call(std::stoull(extra_identity->data),"client_callback","untrusted extra",1s,[&](auto r){pushed=std::move(r);});
    until([&]{return pushed.has_value();});CHECK(pushed->status==fsnet::Status::rejected);
    std::optional<fsnet::Result> pending;client->request("wait","",[&](auto r){pending=std::move(r);});
    client->disconnect();CHECK(pending && pending->status==fsnet::Status::disconnected && !client->entity(owned.handle));
    CHECK(!script_host->attribute_snapshot(owned.handle.subscription) && !script_host->has_route("client_callback"));
    std::optional<fsnet::Result> stopped_host;script_host->call(extra_peer,"wait","",1s,[&](auto result){stopped_host=std::move(result);});
    CHECK(stopped_host && stopped_host->status==fsnet::Status::disconnected);
    auto previous_generation=owned.handle.generation;
    // Callback reconnect is queued until the old poll/teardown is finished.
    client->on_event([&](const auto& event){events.push_back(event);if(event.kind==fsnet::ClientEvent::Kind::disconnected)client->connect({"127.0.0.1",port,{}});});
    client->connect({"127.0.0.1",port,{}});until([&]{return client->state()==fsnet::ClientState::connected;});
    reply.reset();client->request("login","",[&](auto r){reply=std::move(r);});until([&]{return reply && client->controlled();});
    CHECK(client->controlled()->handle.generation>previous_generation);
    reply.reset();client->write(owned.handle,{{"text",std::string("stale")}},[&](auto r){reply=std::move(r);});CHECK(reply->status==fsnet::Status::rejected);
    auto new_bridge=client->script_host();
    new_bridge->bind_context("client_callback",{false,true,false},[](const auto&,std::string,fsnet::Reply reply){reply({fsnet::Status::ok,"new session"});});
    script_host->unbind("client_callback");
    pushed.reset();server.call(physical,"client_callback","",1s,[&](auto r){pushed=std::move(r);});until([&]{return pushed.has_value();});CHECK(pushed->data=="new session");
    auto generation=client->stats().generation;server.close(physical);
    until([&]{return client->stats().generation>generation && client->state()==fsnet::ClientState::connected;});
    // Destruction inside an event cannot leave dangling Node/host callbacks.
    client->on_event([&](const auto& event){if(event.kind==fsnet::ClientEvent::Kind::enter)client.reset();});
    client->request("login","",[](auto){});until([&]{return !client;});
    for(int i=0;i<10;++i)pump();
    script_host.reset();
    client=std::make_unique<fsnet::Client>(options);
    client->on_event([](const auto& event){if(event.kind==fsnet::ClientEvent::Kind::connected)throw std::runtime_error("callback marker");});
    client->connect({"127.0.0.1",port,{}});until([&]{return client->state()==fsnet::ClientState::disconnected;});
    CHECK(client->last_error()=="callback marker");client.reset();
    fsnet::Node frozen("sdk-test");auto frozen_port=frozen.listen("127.0.0.1",0);options.connect_timeout=20ms;
    client=std::make_unique<fsnet::Client>(options);fsnet::Status failure=fsnet::Status::ok;
    client->on_event([&](const auto& event){if(event.kind==fsnet::ClientEvent::Kind::disconnected)failure=event.status;});
    client->connect({"127.0.0.1",frozen_port,{}});until([&]{return client->state()==fsnet::ClientState::disconnected;});
    CHECK(failure==fsnet::Status::timeout);client.reset();
    replication.stop();host->stop();
    std::cout<<"PASS client SDK: handshake, automatic views, shared snapshots, binary RPC, ACL, stale handles, cleanup, callback reconnect and destruction\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}

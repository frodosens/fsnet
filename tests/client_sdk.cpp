#include "fsnet/client.hpp"
#include "fsnet/entity_host.hpp"
#include "fsnet/attribute_replication.hpp"
#include "fsnet/entity_message.hpp"
#include <iostream>
#include <thread>
using namespace fsnet;using namespace std::chrono_literals;
#define CHECK(x) do{if(!(x))throw std::runtime_error(#x);}while(false)
int main(){try{
    Node server("sdk");std::uint64_t accepted=0;
    server.identify([](auto,const auto&)->std::optional<CallIdentity>{return CallIdentity{Caller::client,"same-account"};});
    server.bind_rpc("system.test.peer",{true,false,false},[&](const CallIdentity& who,std::string,Reply reply){accepted=who.peer;reply({Status::ok,{}});});
    auto db=memory_database();auto host=std::make_shared<EntityScriptHost>(std::make_shared<NodeScriptHost>(server),db,"sdk/boot");
    EntitySchema schema;schema.attributes.emplace("x",AttributeDefinition{AttributeType::integer,std::int64_t(1),Access::read_write,Access::read_write});
    host->define_schema("Actor",schema);auto initial=encode_attributes({0,0,true,{{"x",std::int64_t(1)}}});
    host->bind_stateful_context("Actor.echo","Actor",initial,{true,false,true},[](const auto&,std::string input,Reply reply){
        auto entity=parse_entity_message_view(input);auto state=parse_state_message_view(entity->payload);reply({Status::ok,state_message(state->state,state->payload)});
    });
    AttributeReplication replication(server,host);ClientOptions options;options.token="sdk";Client client(options);
    unsigned entered=0,controlled=0;client.on_event([&](const ClientEvent& event){if(event.kind==ClientEvent::Kind::enter)++entered;if(event.kind==ClientEvent::Kind::control && event.controlled)++controlled;});
    auto port=server.listen("127.0.0.1",0);
    auto pump=[&]{server.poll();client.poll();host->poll();replication.poll();};
    auto until=[&](auto done){auto end=std::chrono::steady_clock::now()+5s;while(!done()){pump();CHECK(std::chrono::steady_clock::now()<end);std::this_thread::sleep_for(1ms);}};
    auto wait=[&](auto begin){std::optional<Result> result;begin([&](Result value){result=std::move(value);});until([&]{return result.has_value();});return *result;};
    auto connect=[&]{client.connect({"127.0.0.1",port,{}});until([&]{return client.state()==ClientState::connected;});CHECK(wait([&](auto reply){client.request("system.test.peer","",std::move(reply));}).status==Status::ok);};
    connect();auto connection=server.identity(accepted)->connection;
    auto created=wait([&](auto reply){host->create_entity("Actor",{},{"sdk","actor",connection,"sdk"},std::move(reply));});CHECK(created.status==Status::ok);EntityId id(created.data);
    auto show=[&]{CHECK(wait([&](auto reply){host->entity_visibility(accepted,"Actor",id.str(),1,std::move(reply));}).status==Status::ok);CHECK(wait([&](auto reply){host->entity_visibility(accepted,"Actor",id.str(),3,std::move(reply));}).status==Status::ok);until([&]{return client.controlled().has_value();});};
    show();CHECK(entered==1 && controlled==1);auto old=client.controlled()->handle;auto image=client.controlled()->confirmed;
    std::string bytes("a\0b",3);std::optional<Result> result;
    client.call(id,"echo",bytes,[&](Result value){result=std::move(value);},{"receipt",1s});bytes.assign(1024,'x');
    until([&]{return result.has_value();});CHECK(result->status==Status::ok && result->data==std::string("a\0b",3));
    CHECK(wait([&](auto reply){client.call(EntityKey{"Wrong",id.str()},"echo","",std::move(reply));}).status==Status::rejected);
    bool rejected=false;client.watch({"Actor","arbitrary-name"},[&](auto status,auto){rejected=status==Status::rejected;});CHECK(rejected);
    client.disconnect();CHECK(!client.entity(old));connect();auto replacement=server.identity(accepted)->connection;CHECK(replacement!=connection);
    CHECK(wait([&](auto reply){client.call(id,"echo","",std::move(reply));}).status==Status::rejected);
    CHECK(wait([&](auto reply){host->bind_entity_owner("Actor",id.str(),replacement,std::move(reply));}).status==Status::ok);show();CHECK(client.controlled()->handle.generation!=old.generation);
    CHECK(wait([&](auto reply){client.write(old,{{"x",std::int64_t(8)}},std::move(reply));}).status==Status::rejected);
    CHECK(std::get<std::int64_t>(image->values.at("x"))==1);
    Client invalid(options);invalid.watch({"Actor","invalid"},[](auto,auto){throw std::runtime_error("watch callback");});CHECK(invalid.last_error()=="watch callback");
    client.disconnect();replication.stop();host->stop();server.stop();
    std::cout<<"PASS global SDK: connection, automatic views/control, owned bytes, type assertions, reconnect binding, stale handles and callback containment\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}

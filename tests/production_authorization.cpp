#include "fsnet/attribute_replication.hpp"
#include "fsnet/client_gateway.hpp"
#include "fsnet/entity_message.hpp"
#include "fsnet/replication_wire.hpp"
#include <iostream>
#include <thread>

using namespace fsnet;
using namespace std::chrono_literals;
#define CHECK(c) do { if(!(c)) throw std::runtime_error(std::string(__FILE__)+":"+std::to_string(__LINE__)+" " #c); } while(false)

namespace {
struct ObservedStorage final : Storage {
    std::shared_ptr<Storage> db=memory_database();
    std::size_t writes=0;
    std::optional<Record> get(std::string_view key) override { return db->get(key); }
    bool compare_exchange(std::string_view key,std::uint64_t expected,std::string_view value) override {
        ++writes;return db->compare_exchange(key,expected,value);
    }
    bool erase(std::string_view key,std::uint64_t expected) override { ++writes;return db->erase(key,expected); }
};

struct AuthorizationFixture {
    Node owner{"authorization"},client{"authorization"};
    std::shared_ptr<ObservedStorage> db=std::make_shared<ObservedStorage>();
    std::shared_ptr<EntityScriptHost> host=std::make_shared<EntityScriptHost>(std::make_shared<NodeScriptHost>(owner),db,"owner/boot");
    ConnectionRef connection=ConnectionRef::create();
    unsigned activations=0;
    bool observer=false;
    bool stop_on_observer=false,stop_on_transport_identity=false;
    std::uint64_t owner_peer=0;
    std::unique_ptr<GatewayDelegation> delegation;
    std::unique_ptr<AttributeReplication> replication;
    std::uint64_t peer=0;

    explicit AuthorizationFixture(bool delegated=false) {
        EntitySchema schema;
        schema.attributes.emplace("activated",AttributeDefinition{AttributeType::integer,std::int64_t(0),Access::read_write,Access::read,Visibility::observers});
        schema.attributes.emplace("secret",AttributeDefinition{AttributeType::integer,std::int64_t(7)});
        host->define_schema("Actor",schema);host->define_schema("Other",schema);
        auto initial=encode_attributes({0,0,true,{{"activated",std::int64_t(0)},{"secret",std::int64_t(7)}}});
        auto unchanged=[](const CallIdentity&,std::string data,Reply reply) {
            auto call=parse_entity_message_view(data);auto state=parse_state_message_view(call->payload);
            reply({Status::ok,state_message(state->state,"")});
        };
        host->bind_stateful_context("Actor.noop","Actor",initial,{false,true,false},unchanged);
        host->bind_stateful_context("Actor.$deactivate","Actor",initial,{false,true,false},unchanged);
        host->bind_stateful_context("Actor.$activate","Actor",initial,{false,true,false},[&](const CallIdentity&,std::string data,Reply reply) {
            ++activations;
            auto call=parse_entity_message_view(data);auto state=parse_state_message_view(call->payload);auto image=decode_attributes(state->state);
            ++std::get<std::int64_t>(image.values.at("activated"));++image.revision;
            reply({Status::ok,state_message(encode_attributes(image),"")});
        });
        if(delegated) {
            auto base=[this](auto peer,const auto&)->std::optional<CallIdentity>{
                owner_peer=peer;
                if(stop_on_transport_identity){stop_on_transport_identity=false;delegation->stop();}
                return CallIdentity{Caller::server,"trusted-gateway"};
            };
            delegation=std::make_unique<GatewayDelegation>(owner,base,[](const CallIdentity& who,const auto&){return who.caller==Caller::server&&who.subject=="trusted-gateway";},host);
        } else owner.identify([&](auto peer,const auto&)->std::optional<CallIdentity>{return CallIdentity{Caller::client,"same-subject",peer,connection};});
        replication=std::make_unique<AttributeReplication>(owner,host,[&](const auto&,const auto&,const auto&){if(stop_on_observer)replication->stop();return observer;});
        peer=client.connect("127.0.0.1",owner.listen("127.0.0.1",0));
        until([&]{return client.connected(peer);});
        if(delegated) {
            std::string data="FSCN1";snapshot_detail::blob(data,"same-subject");snapshot_detail::blob(data,connection.key());
            CHECK(call("system.gateway.delegate",std::move(data)).status==Status::ok);
        }
    }
    ~AuthorizationFixture() {
        replication->stop();if(delegation)delegation->stop();host->stop();client.stop();owner.stop();
    }
    void poll() {owner.poll();client.poll();host->poll();replication->poll();if(delegation)delegation->poll();}
    template<class F> void until(F done) {
        auto end=std::chrono::steady_clock::now()+3s;
        while(!done()){poll();CHECK(std::chrono::steady_clock::now()<end);std::this_thread::sleep_for(1ms);}
    }
    template<class F> Result wait(F begin) {
        std::optional<Result> result;begin([&](Result value){result=std::move(value);});until([&]{return bool(result);});return std::move(*result);
    }
    Result call(std::string route,std::string data) {
        return wait([&](Reply reply){client.call(peer,std::move(route),std::move(data),2s,std::move(reply));});
    }
    EntityId unloaded() {
        auto created=wait([&](Reply reply){host->create_entity("Actor",{},{"auth-test","entity",ConnectionRef::create(),"owner"},std::move(reply));});
        CHECK(created.status==Status::ok);EntityId id(created.data);
        CHECK(wait([&](Reply reply){host->control_entity("Actor",id.str(),false,std::move(reply));}).status==Status::ok);
        CHECK(!host->active("Actor",id.str()));return id;
    }
    void assert_unchanged(EntityId id,const Record& before,std::size_t writes,unsigned calls) {
        auto after=db->get(entity_record_key(id));CHECK(after && after->value==before.value && after->version==before.version);
        CHECK(db->writes==writes && activations==calls && !host->active("Actor",id.str()));
    }
};

void unauthorized_watch_does_not_activate() {
    AuthorizationFixture fixture;
    auto id=fixture.unloaded();auto before=*fixture.db->get(entity_record_key(id));
    auto writes=fixture.db->writes;auto calls=fixture.activations;
    CHECK(fixture.call("system.attributes.watch",encode_entity_key("Actor",id.str())).status==Status::rejected);
    fixture.assert_unchanged(id,before,writes,calls);
    CHECK(fixture.call("system.attributes.watch",encode_entity_key("Other",id.str())).status==Status::rejected);
    fixture.assert_unchanged(id,before,writes,calls);
    CHECK(fixture.call("system.attributes.watch",encode_entity_key("Actor","unallocated")).status==Status::rejected);
    fixture.assert_unchanged(id,before,writes,calls);

    fixture.observer=true;
    auto watch=fixture.call("system.attributes.watch",encode_entity_key("Actor",id.str()));CHECK(watch.status==Status::ok);
    auto packet=decode_replication_packet(watch.data);CHECK(packet.update && packet.update->values.contains("activated"));
    CHECK(!packet.update->values.contains("secret") && fixture.activations==calls+1);
    std::string token;for(unsigned i=8;i;--i)token+=char(packet.subscription>>((i-1)*8));
    CHECK(fixture.call("system.attributes.unwatch",std::move(token)).status==Status::ok);
    CHECK(fixture.wait([&](Reply reply){fixture.host->control_entity("Actor",id.str(),false,std::move(reply));}).status==Status::ok);
    fixture.observer=false;
    CHECK(fixture.wait([&](Reply reply){fixture.host->bind_entity_owner("Actor",id.str(),fixture.connection,std::move(reply));}).status==Status::ok);
    CHECK(!fixture.host->active("Actor",id.str()));
    watch=fixture.call("system.attributes.watch",encode_entity_key("Actor",id.str()));CHECK(watch.status==Status::ok);
    packet=decode_replication_packet(watch.data);CHECK(packet.update && packet.update->values.contains("secret"));
    CHECK(fixture.host->active("Actor",id.str()));
}

void unauthorized_restore_does_not_activate() {
    AuthorizationFixture fixture(true);
    auto id=fixture.unloaded();auto before=*fixture.db->get(entity_record_key(id));
    auto writes=fixture.db->writes;auto calls=fixture.activations;
    auto restore=[&](const std::string& type){return fixture.call("system.gateway.restore",encode_entity_key(type,id.str())+char(0));};
    CHECK(restore("Actor").status==Status::rejected);fixture.assert_unchanged(id,before,writes,calls);
    CHECK(restore("Other").status==Status::rejected);fixture.assert_unchanged(id,before,writes,calls);
    CHECK(fixture.wait([&](Reply reply){fixture.host->bind_entity_owner("Actor",id.str(),fixture.connection,std::move(reply));}).status==Status::ok);
    CHECK(!fixture.host->active("Actor",id.str()));
    CHECK(restore("Actor").status==Status::ok);
    CHECK(fixture.host->active("Actor",id.str()) && fixture.activations==calls+1);
}

void callbacks_can_stop_authorization_services() {
    {
        AuthorizationFixture fixture;
        auto id=fixture.unloaded();auto before=*fixture.db->get(entity_record_key(id));
        auto writes=fixture.db->writes;auto activations=fixture.activations;
        fixture.observer=true;fixture.stop_on_observer=true;
        auto result=fixture.call("system.attributes.watch",encode_entity_key("Actor",id.str()));
        CHECK(result.status!=Status::ok);
        fixture.assert_unchanged(id,before,writes,activations);
    }
    for(bool through_poll:{false,true}) {
        AuthorizationFixture fixture(true);
        fixture.stop_on_transport_identity=true;
        if(through_poll)fixture.delegation->poll();
        else CHECK(!fixture.owner.identity(fixture.owner_peer));
        CHECK(!fixture.stop_on_transport_identity);
        fixture.delegation->poll();
    }
}

enum class RevokeAt { poll, notification, reply, request, resolver_stop, live_stop };
void replaced_connection_loses_gateway_access(RevokeAt boundary) {
    Node owner("auth-rebind"),front("auth-rebind"),back("auth-rebind"),client("auth-rebind");
    auto db=memory_database();auto host=std::make_shared<EntityScriptHost>(std::make_shared<NodeScriptHost>(owner),db,"owner/boot");
    EntitySchema schema;schema.attributes.emplace("secret",AttributeDefinition{AttributeType::integer,std::int64_t(1)});host->define_schema("Actor",schema);
    auto initial=encode_attributes({0,0,true,{{"secret",std::int64_t(1)}}});
    host->bind_stateful_context("Actor.bump","Actor",initial,{false,true,false},[](const CallIdentity&,std::string data,Reply reply) {
        auto call=parse_entity_message_view(data);auto state=parse_state_message_view(call->payload);auto image=decode_attributes(state->state);
        ++std::get<std::int64_t>(image.values.at("secret"));++image.revision;reply({Status::ok,state_message(encode_attributes(image),"")});
    });
    Reply delayed;std::string delayed_input;
    host->bind_stateful_context("Actor.slow","Actor",initial,{true,true,false},[&](const auto&,std::string data,Reply reply){delayed_input=std::move(data);delayed=std::move(reply);});
    auto base=[](auto,const auto&)->std::optional<CallIdentity>{return CallIdentity{Caller::server,"trusted-gateway"};};
    GatewayDelegation delegation(owner,base,[](const CallIdentity& who,const auto&){return who.caller==Caller::server&&who.subject=="trusted-gateway";},host);
    AttributeReplication replication(owner,host);
    auto logical=ConnectionRef::create();auto original=logical;
    ClientGateway* gateway_pointer=nullptr;unsigned stop_after_resolve=0;
    front.identify([&](auto peer,const auto&)->std::optional<CallIdentity>{
        if(stop_after_resolve && --stop_after_resolve==0)gateway_pointer->stop();
        return CallIdentity{Caller::client,"same-subject",peer,logical};
    });
    GatewayLimits limits;limits.refresh=5ms;
    ClientGateway gateway(front,back,db,{{"owner","127.0.0.1",owner.listen("127.0.0.1",0),{"Actor"}}},limits);
    gateway_pointer=&gateway;
    gateway.bind_entity("Actor",{"slow"});
    auto peer=client.connect("127.0.0.1",front.listen("127.0.0.1",0));
    unsigned private_notices=0;
    client.on_notification("system.attributes.update",[&](auto,const auto&,std::string data){auto message=decode_replication_packet(data);if(message.update && message.update->values.contains("secret"))++private_notices;});
    auto poll=[&](bool gateway_poll=true){owner.poll();front.poll();back.poll();client.poll();host->poll();replication.poll();delegation.poll();if(gateway_poll)gateway.poll();};
    auto until=[&](auto done,bool gateway_poll=true){auto end=std::chrono::steady_clock::now()+3s;while(!done()){poll(gateway_poll);CHECK(std::chrono::steady_clock::now()<end);std::this_thread::sleep_for(1ms);}};
    auto wait=[&](auto begin,bool gateway_poll=true){std::optional<Result> result;begin([&](Result value){result=std::move(value);});until([&]{return bool(result);},gateway_poll);return std::move(*result);};
    auto created=wait([&](Reply reply){host->create_entity("Actor",{},{"auth-rebind","entity",original,"owner"},std::move(reply));});CHECK(created.status==Status::ok);EntityId id(created.data);
    auto watch=wait([&](Reply reply){client.call(peer,"system.attributes.watch",encode_entity_key("Actor",id.str()),2s,std::move(reply));});CHECK(watch.status==Status::ok);
    std::optional<Result> response;
    if(boundary==RevokeAt::reply) {
        client.call(peer,entity_call_route,entity_call_message(id,"slow",""),2s,[&](Result value){response=std::move(value);});
        until([&]{return bool(delayed);});
    }
    bool stop_boundary=boundary==RevokeAt::resolver_stop || boundary==RevokeAt::live_stop;
    if(stop_boundary){stop_after_resolve=boundary==RevokeAt::resolver_stop?1:2;gateway.poll();CHECK(stop_after_resolve==0);}
    else {logical=ConnectionRef::create();CHECK(logical!=original);}
    if(boundary==RevokeAt::poll) gateway.poll();
    else if(boundary==RevokeAt::reply) {
        auto call=parse_entity_message_view(delayed_input);auto state=parse_state_message_view(call->payload);
        auto reply=std::move(delayed);reply({Status::ok,state_message(state->state,"private-reply")});
    } else if(boundary==RevokeAt::request) {
        client.call(peer,entity_call_route,entity_call_message(id,"slow",""),2s,[&](Result value){response=std::move(value);});
    }
    if(boundary==RevokeAt::notification || boundary==RevokeAt::poll) {
        auto bump=wait([&](Reply reply){host->call_entity(entity_call_route,entity_call_message(id,"bump",""),2s,std::move(reply));},false);
        CHECK(bump.status==Status::ok);
    }
    until([&]{return gateway.stats().clients==0 && gateway.stats().subscriptions==0 && gateway.stats().links==0 && gateway.stats().pending==0;},false);
    if(!stop_boundary)until([&]{return !client.connected(peer);},false);
    CHECK(private_notices==0);
    if(boundary==RevokeAt::reply || boundary==RevokeAt::request) {
        until([&]{return bool(response);},false);CHECK(response->status!=Status::ok && response->data!="private-reply");
    }
    CHECK(!delayed);
    gateway.stop();replication.stop();delegation.stop();host->stop();owner.stop();front.stop();back.stop();client.stop();
}
}

int main() {
    try {
        unauthorized_watch_does_not_activate();unauthorized_restore_does_not_activate();callbacks_can_stop_authorization_services();
        for(auto boundary:{RevokeAt::poll,RevokeAt::notification,RevokeAt::reply,RevokeAt::request,RevokeAt::resolver_stop,RevokeAt::live_stop})replaced_connection_loses_gateway_access(boundary);
        std::cout<<"PASS authorization: rejection before activation, metadata ownership, and logical connection revocation at all Gateway boundaries\n";
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}

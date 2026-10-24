#pragma once
#include "fsnet/script.hpp"
#include "fsnet/attribute_replication.hpp"
#include <unordered_map>
#include <deque>

namespace fsnet {
// Owner-thread client adapter. One per Node, bound to an authenticated server peer.
// Node outlives this host and the LuaEngine. Poll drives subscription renewal.
class ClientScriptHost final : public ScriptHost {
    std::shared_ptr<Node> node_owner_;
    Node& node_;
    std::uint64_t server_peer_;
    std::deque<EntityPresence> events_;
    bool event_overflow_=false,queue_events_=true,stopped_=false;
    std::vector<std::uint64_t> extra_peers_;
    AttributeClient::Changed changed_;
    AttributeClient::Presence presence_;
    static Node& require_node(const std::shared_ptr<Node>& node) { if(!node)throw std::invalid_argument("missing node");return *node; }
    AttributeClient attributes_;
    void event(const EntityPresence& value) {
        if(event_overflow_ || !queue_events_) return;
        if(events_.size()>=512) { events_.clear(); event_overflow_=true; node_.close(server_peer_); return; }
        events_.push_back(value);
    }
public:
    ClientScriptHost(Node& node, std::uint64_t server_peer) : node_(node), server_peer_(server_peer), attributes_(node,server_peer,
        [this](std::uint64_t id,Status status,const AttributeReplica* replica) { if(changed_){auto callback=changed_;callback(id,status,replica);} },
        [this](const EntityPresence& value) { event(value);if(presence_){auto callback=presence_;callback(value);} }) {}
    ClientScriptHost(std::shared_ptr<Node> node,std::uint64_t peer,bool queue_events)
      : ClientScriptHost(require_node(node),peer) { node_owner_=std::move(node);queue_events_=queue_events; }
    void observe(AttributeClient::Changed changed,AttributeClient::Presence presence) { changed_=std::move(changed);presence_=std::move(presence); }
    void enable_entity_events() {
        if(queue_events_)return;
        queue_events_=true;
        for(const auto& p:attributes_.projection_infos())if(p.automatic)event({EntityPresence::Kind::enter,p.type,p.id,p.subscription,p.owned,p.controlled});
    }
    std::optional<AttributeProjectionInfo> projection_info(std::uint64_t id) const { return attributes_.projection_info(id); }
    ~ClientScriptHost() override { stop(); }
    bool has_route(const std::string& route) const override { return node_.has_route(route); }
    void bind(std::string route, Handler handler) override { if(stopped_)throw std::logic_error("client host stopped");node_.bind(std::move(route),std::move(handler)); }
    void bind_context(std::string route,RpcPolicy policy,ContextHandler handler) override { if(stopped_)throw std::logic_error("client host stopped");node_.bind_rpc(std::move(route),policy,std::move(handler)); }
    void rpc_policy(const std::string& route,RpcPolicy policy) override { node_.rpc_policy(route,policy); }
    void unbind(const std::string& route) override { node_.unbind(route); }
    std::uint64_t connect(const std::string& host,std::uint16_t port) override {
        if(stopped_)throw std::logic_error("client host stopped");
        auto peer=node_.connect(host,port);try{extra_peers_.push_back(peer);}catch(...){node_.close(peer);throw;}return peer;
    }
    void call(std::uint64_t peer,std::string route,std::string data,std::chrono::milliseconds timeout,Reply reply) override {
        if(stopped_)return reply({Status::disconnected,{}});
        node_.call(peer,std::move(route),std::move(data),timeout,std::move(reply));
    }
    // Logical references target the configured owner or authenticated Gateway.
    void call_entity(std::string route,std::string data,std::chrono::milliseconds timeout,Reply reply) override {
        if(stopped_)return reply({Status::disconnected,{}});
        node_.call(server_peer_,std::move(route),std::move(data),timeout,std::move(reply));
    }
    void watch_attributes(std::string type,std::string id,AttributeReady ready,std::chrono::milliseconds timeout) override {
        attributes_.watch(std::move(type),std::move(id),[this,ready=std::move(ready)](Status status,std::uint64_t subscription) {
            auto* replica=attributes_.get(subscription);
            if(status==Status::ok && !replica) status=Status::disconnected;
            ready(status,subscription);
        },timeout);
    }
    std::shared_ptr<const AttributeUpdate> attribute_snapshot(std::uint64_t subscription) const override {
        auto* replica=attributes_.get(subscription);
        return replica?replica->snapshot():nullptr;
    }
    void write_attributes(std::uint64_t subscription,EntityAttributes::Values values,Reply reply,
                          AttributeWriteOptions options,std::chrono::milliseconds timeout) override {
        attributes_.write(subscription,std::move(values),[reply=std::move(reply)](Status status) { reply({status,{}}); },std::move(options),timeout);
    }
    void unwatch_attributes(std::uint64_t subscription) override { attributes_.unwatch(subscription); }
    std::optional<EntityPresence> pop_entity_event() override {
        if(event_overflow_) throw std::runtime_error("entity event queue overflow");
        if(events_.empty()) return {};
        auto value=std::move(events_.front()); events_.pop_front(); return value;
    }
    void stop_entity_events() override { attributes_.stop_entities(); events_.clear(); }
    void poll() { attributes_.poll(); }
    void stop() { if(std::exchange(stopped_,true))return;attributes_.stop();auto peers=std::move(extra_peers_);for(auto peer:peers)node_.close(peer); }
};
}

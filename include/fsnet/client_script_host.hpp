#pragma once
#include "fsnet/script.hpp"
#include "fsnet/attribute_replication.hpp"
#include <unordered_map>

namespace fsnet {
// Owner-thread client adapter. One per Node, bound to an authenticated server peer.
// Node outlives this host and the LuaEngine. Poll drives subscription renewal.
class ClientScriptHost final : public ScriptHost {
    Node& node_;
    std::unordered_map<std::uint64_t,std::shared_ptr<const AttributeUpdate>> images_;
    AttributeClient attributes_;
    void image(std::uint64_t id,Status status,const AttributeReplica* replica) {
        if(status!=Status::ok || !replica) { images_.erase(id); return; }
        images_[id]=std::make_shared<const AttributeUpdate>(AttributeUpdate{0,replica->revision(),true,replica->values()});
    }
public:
    ClientScriptHost(Node& node, std::uint64_t server_peer) : node_(node), attributes_(node,server_peer,
        [this](std::uint64_t id,Status status,const AttributeReplica* replica) { image(id,status,replica); }) {}
    bool has_route(const std::string& route) const override { return node_.has_route(route); }
    void bind(std::string route, Handler handler) override { node_.bind(std::move(route),std::move(handler)); }
    void bind_context(std::string route,RpcPolicy policy,ContextHandler handler) override { node_.bind_rpc(std::move(route),policy,std::move(handler)); }
    void rpc_policy(const std::string& route,RpcPolicy policy) override { node_.rpc_policy(route,policy); }
    void unbind(const std::string& route) override { node_.unbind(route); }
    std::uint64_t connect(const std::string& host,std::uint16_t port) override { return node_.connect(host,port); }
    void call(std::uint64_t peer,std::string route,std::string data,std::chrono::milliseconds timeout,Reply reply) override {
        node_.call(peer,std::move(route),std::move(data),timeout,std::move(reply));
    }
    void watch_attributes(std::string type,std::string id,AttributeReady ready,std::chrono::milliseconds timeout) override {
        attributes_.watch(std::move(type),std::move(id),[this,ready=std::move(ready)](Status status,std::uint64_t subscription) {
            auto* replica=attributes_.get(subscription);
            if(status==Status::ok && !replica) status=Status::disconnected;
            image(subscription,status,replica); ready(status,subscription);
        },timeout);
    }
    std::shared_ptr<const AttributeUpdate> attribute_snapshot(std::uint64_t subscription) const override {
        auto it=images_.find(subscription); return it==images_.end()?nullptr:it->second;
    }
    void write_attributes(std::uint64_t subscription,EntityAttributes::Values values,Reply reply,
                          AttributeWriteOptions options,std::chrono::milliseconds timeout) override {
        attributes_.write(subscription,std::move(values),[reply=std::move(reply)](Status status) { reply({status,{}}); },std::move(options),timeout);
    }
    void unwatch_attributes(std::uint64_t subscription) override { attributes_.unwatch(subscription); }
    void poll() { attributes_.poll(); }
    void stop() { attributes_.stop(); }
};
}

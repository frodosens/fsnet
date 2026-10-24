#pragma once
#include "fsnet/async_database.hpp"
#include "fsnet/script.hpp"
#include <vector>

namespace fsnet {
struct EntityEndpoint {
    std::string node, address;
    std::uint16_t port;
    std::vector<std::string> types;
};
struct EntityRouterLimits { std::size_t requests = 256, bytes = 4 * 1024 * 1024; };
// Static membership + authoritative database ownership, not health-based exclusive
// ownership. Owner IDs use "stable-node/unique-boot-id". Calls with a request ID
// may retry until the deadline; receipt retention limits still apply.
class EntityRouter {
public:
    EntityRouter(Node&, std::shared_ptr<Database>, std::vector<EntityEndpoint>, EntityRouterLimits = {});
    ~EntityRouter();
    void call(std::string type, std::string id, std::string method, std::string payload,
              std::chrono::milliseconds timeout, Reply, std::string request_id = {});
    std::size_t poll(std::size_t budget = 64);
    std::size_t pending() const;
    void stop();
private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};
class RoutedScriptHost final : public ScriptHost {
    std::shared_ptr<ScriptHost> parent_;
    EntityRouter& router_;
public:
    RoutedScriptHost(std::shared_ptr<ScriptHost> parent, EntityRouter& router) : parent_(std::move(parent)), router_(router) {
        if (!parent_) throw std::invalid_argument("script host required");
    }
    bool has_route(const std::string& r) const override { return parent_->has_route(r); }
    void bind(std::string r, Handler h) override { parent_->bind(std::move(r), std::move(h)); }
    void bind_stateful(std::string r, std::string t, std::string s, Handler h) override {
        parent_->bind_stateful(std::move(r), std::move(t), std::move(s), std::move(h));
    }
    void define_schema(std::string t, EntitySchema s) override { parent_->define_schema(std::move(t), std::move(s)); }
    void spawn_entity(std::string t,std::string id,EntityAttributes::Values values,Reply reply) override {
        parent_->spawn_entity(std::move(t),std::move(id),std::move(values),std::move(reply));
    }
    void control_entity(std::string t,std::string id,bool destroy,Reply reply) override { parent_->control_entity(std::move(t),std::move(id),destroy,std::move(reply)); }
    void entity_visibility(std::uint64_t peer,std::string t,std::string id,unsigned action,Reply reply) override {
        parent_->entity_visibility(peer,std::move(t),std::move(id),action,std::move(reply));
    }
    std::optional<EntityPresence> pop_entity_event() override { return parent_->pop_entity_event(); }
    void stop_entity_events() override { parent_->stop_entity_events(); }
    void forget_schema(const std::string& t) override { parent_->forget_schema(t); }
    std::string entity_owner(const std::string& t, const std::string& id) const override { return parent_->entity_owner(t,id); }
    void bind_context(std::string r, RpcPolicy p, ContextHandler h) override { parent_->bind_context(std::move(r), p, std::move(h)); }
    void bind_stateful_context(std::string r, std::string t, std::string s, RpcPolicy p, ContextHandler h) override {
        parent_->bind_stateful_context(std::move(r), std::move(t), std::move(s), p, std::move(h));
    }
    void rpc_policy(const std::string& r, RpcPolicy p) override { parent_->rpc_policy(r, p); }
    void unbind(const std::string& r) override { parent_->unbind(r); }
    std::uint64_t connect(const std::string& a, std::uint16_t p) override { return parent_->connect(a, p); }
    void call(std::uint64_t p, std::string r, std::string d, std::chrono::milliseconds t, Reply f) override {
        parent_->call(p, std::move(r), std::move(d), t, std::move(f));
    }
    void call_entity(std::string route, std::string data, std::chrono::milliseconds timeout, Reply reply) override;
};
}

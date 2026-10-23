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
    void unbind(const std::string& r) override { parent_->unbind(r); }
    std::uint64_t connect(const std::string& a, std::uint16_t p) override { return parent_->connect(a, p); }
    void call(std::uint64_t p, std::string r, std::string d, std::chrono::milliseconds t, Reply f) override {
        parent_->call(p, std::move(r), std::move(d), t, std::move(f));
    }
    void call_entity(std::string route, std::string data, std::chrono::milliseconds timeout, Reply reply) override;
};
}

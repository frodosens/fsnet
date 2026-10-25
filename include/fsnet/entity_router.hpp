#pragma once
#include "fsnet/async_database.hpp"
#include "fsnet/entity_directory.hpp"
#include "fsnet/script.hpp"
#include <vector>

namespace fsnet {
struct EntityEndpoint {
    std::string node, address;
    std::uint16_t port;
    std::vector<std::string> types;
    std::string tls_peer_name{};
};
void validate_entity_endpoints(const std::vector<EntityEndpoint>&);
std::optional<std::size_t> select_entity_endpoint(const std::vector<EntityEndpoint>&,const std::string& type,
    const std::string& id,const std::optional<EntityLocation>&,std::int64_t now_ms,
    const std::function<bool(std::size_t)>& eligible = {});
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
    void legacy_call(std::string type,std::string id,std::string method,std::string payload,
                     std::chrono::milliseconds,Reply,std::string request_id={});
    void call(EntityId,std::string_view method,std::string_view payload,std::chrono::milliseconds,Reply,std::string_view request_id={},std::string_view expected_type={});
    void call_encoded(std::string message,std::chrono::milliseconds,Reply); // takes ownership before returning
    void bind_owner(std::string type,std::string id,ConnectionRef,std::chrono::milliseconds,Reply);
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
    void enable_legacy_routes()override {parent_->enable_legacy_routes();}
    void bind(std::string r, Handler h) override { parent_->bind(std::move(r), std::move(h)); }
    void bind_legacy_stateful(std::string r, std::string t, std::string s, Handler h) override {
        parent_->bind_legacy_stateful(std::move(r), std::move(t), std::move(s), std::move(h));
    }
    void define_schema(std::string t, EntitySchema s) override { parent_->define_schema(std::move(t), std::move(s)); }
    void legacy_spawn_entity(std::string t,std::string id,EntityAttributes::Values values,Reply reply) override {
        parent_->legacy_spawn_entity(std::move(t),std::move(id),std::move(values),std::move(reply));
    }
    void create_entity(std::string type,EntityAttributes::Values values,EntitySpawnOptions options,Reply reply) override {
        parent_->create_entity(std::move(type),std::move(values),std::move(options),std::move(reply));
    }
    void bind_entity_owner(std::string type,std::string id,ConnectionRef owner,Reply reply)override {router_.bind_owner(std::move(type),std::move(id),std::move(owner),std::chrono::seconds(30),std::move(reply));}
    std::string schedule_timer(const std::string& type,const std::string& id,EntityTimerSpec spec)override{return parent_->schedule_timer(type,id,std::move(spec));}
    std::string timer_key(const std::string& type,const std::string& id)const override{return parent_->timer_key(type,id);}
    bool cancel_timer(const std::string& type,const std::string& id,const std::string& key)override{return parent_->cancel_timer(type,id,key);}
    std::optional<EntityTimer> entity_timer(const std::string& type,const std::string& id,const std::string& key)const override{return parent_->entity_timer(type,id,key);}
    void send_entity(std::string st,std::string sid,std::string t,std::string id,std::string m,std::string p,Reply r) override { parent_->send_entity(std::move(st),std::move(sid),std::move(t),std::move(id),std::move(m),std::move(p),std::move(r)); }
    std::optional<DeliveryMessage> delivery_status(const std::string& t,const std::string& id,const std::string& ticket) const override { return parent_->delivery_status(t,id,ticket); }
    bool forget_delivery(const std::string& t,const std::string& id,const std::string& ticket) override { return parent_->forget_delivery(t,id,ticket); }
    void transfer_entity(std::string t,std::string id,std::string target,Reply reply) override { parent_->transfer_entity(std::move(t),std::move(id),std::move(target),std::move(reply)); }
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

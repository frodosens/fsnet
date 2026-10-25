#pragma once
#include "fsnet/node.hpp"
#include "fsnet/entity_delivery_state.hpp"
#include "fsnet/attribute_wire.hpp"
#include "fsnet/entity_catalog.hpp"
#include <cstddef>
#include <string>
#include <string_view>
#include <stdexcept>
#include <optional>

namespace fsnet {
// Language-neutral host contract. Payloads are owned binary strings; script
// values, VM pointers, and C++ entities never cross this boundary. All methods
// and callbacks execute on the owning runtime thread.
struct EntityPresence {
    enum class Kind { enter=1, leave=2, control=3 } kind;
    std::string type,id;
    std::uint64_t subscription=0;
    bool owned=false, controlled=false;
};
class ScriptHost {
public:
    virtual ~ScriptHost() = default;
    virtual void enable_legacy_routes() {} // explicit fixture/compatibility adapter
    virtual bool has_route(const std::string& route) const = 0;
    virtual void bind(std::string route, Handler handler) = 0;
    virtual void bind_legacy_stateful(std::string, std::string, std::string, Handler) {
        throw std::logic_error("stateful entities require EntityScriptHost");
    }
    virtual void define_schema(std::string, EntitySchema) {
        throw std::logic_error("entity schemas require an entity host");
    }
    virtual void legacy_spawn_entity(std::string, std::string, EntityAttributes::Values, Reply reply) { reply({Status::rejected,{}}); }
    virtual void create_entity(std::string,EntityAttributes::Values,EntitySpawnOptions,Reply reply) {reply({Status::rejected,{}});}
    virtual void bind_entity_owner(std::string,std::string,ConnectionRef,Reply reply) {reply({Status::rejected,{}});}
    virtual std::string schedule_timer(const std::string&,const std::string&,EntityTimerSpec){throw std::logic_error("timers require a managed durable entity");}
    virtual std::string timer_key(const std::string&,const std::string&)const{throw std::logic_error("timers require a managed durable entity");}
    virtual bool cancel_timer(const std::string&,const std::string&,const std::string&){throw std::logic_error("timers require a managed durable entity");}
    virtual std::optional<EntityTimer> entity_timer(const std::string&,const std::string&,const std::string&)const {return {};}
    virtual void send_entity(std::string,std::string,std::string,std::string,std::string,std::string,Reply reply) { reply({Status::rejected,{}}); }
    virtual std::optional<DeliveryMessage> delivery_status(const std::string&,const std::string&,const std::string&) const { return {}; }
    virtual bool forget_delivery(const std::string&,const std::string&,const std::string&) { return false; }
    virtual void transfer_entity(std::string, std::string, std::string, Reply reply) { reply({Status::rejected,{}}); }
    virtual void control_entity(std::string, std::string, bool, Reply reply) { reply({Status::rejected,{}}); }
    virtual void entity_visibility(std::uint64_t, std::string, std::string, unsigned, Reply reply) { reply({Status::rejected,{}}); }
    virtual std::optional<EntityPresence> pop_entity_event() { return {}; }
    virtual void stop_entity_events() {}
    virtual void forget_schema(const std::string&) {}
    virtual std::string entity_owner(const std::string&, const std::string&) const { return {}; }
    using AttributeReady = std::function<void(Status, std::uint64_t)>;
    virtual void watch_attributes(std::string, std::string, AttributeReady ready, std::chrono::milliseconds) {
        ready(Status::rejected, 0);
    }
    virtual std::shared_ptr<const AttributeUpdate> attribute_snapshot(std::uint64_t) const { return {}; }
    virtual void write_attributes(std::uint64_t, EntityAttributes::Values, Reply reply,
                                  AttributeWriteOptions, std::chrono::milliseconds) { reply({Status::rejected,{}}); }
    virtual void unwatch_attributes(std::uint64_t) {}
    virtual void bind_context(std::string, RpcPolicy, ContextHandler) {
        throw std::logic_error("script host does not support caller context");
    }
    virtual void bind_stateful_context(std::string, std::string, std::string, RpcPolicy, ContextHandler) {
        throw std::logic_error("script host does not support stateful caller context");
    }
    virtual void rpc_policy(const std::string&, RpcPolicy) {
        throw std::logic_error("script host does not support RPC policies");
    }
    virtual void unbind(const std::string& route) = 0;
    virtual void call_entity(std::string, std::string, std::chrono::milliseconds, Reply reply) {
        reply({Status::rejected, {}});
    }
    virtual std::uint64_t connect(const std::string& address, std::uint16_t port) = 0;
    virtual void call(std::uint64_t peer, std::string route, std::string data,
                      std::chrono::milliseconds timeout, Reply reply) = 0;
};
// Node must outlive its host and adapters. A host never owns or stops the Node.
class NodeScriptHost final : public ScriptHost {
    Node& node_;
public:
    explicit NodeScriptHost(Node& node) : node_(node) {}
    bool has_route(const std::string& route) const override { return node_.has_route(route); }
    void bind(std::string route, Handler handler) override { node_.bind(std::move(route), std::move(handler)); }
    void bind_context(std::string route, RpcPolicy policy, ContextHandler handler) override {
        node_.bind_rpc(std::move(route), policy, std::move(handler));
    }
    void rpc_policy(const std::string& route, RpcPolicy policy) override { node_.rpc_policy(route, policy); }
    void unbind(const std::string& route) override { node_.unbind(route); }
    std::uint64_t connect(const std::string& address, std::uint16_t port) override { return node_.connect(address, port); }
    void call(std::uint64_t peer, std::string route, std::string data,
              std::chrono::milliseconds timeout, Reply reply) override {
        node_.call(peer, std::move(route), std::move(data), timeout, std::move(reply));
    }
};
class ScriptEngine {
public:
    virtual ~ScriptEngine() = default;
    virtual void load(std::string_view source, const std::string& name) = 0;
    virtual std::size_t poll(std::size_t work_limit = 64) = 0;
    virtual void stop() = 0;
};
}

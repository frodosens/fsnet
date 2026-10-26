#pragma once
#include "fsnet/async_database.hpp"
#include "fsnet/script.hpp"
#include "fsnet/attribute_wire.hpp"
#include <optional>

namespace fsnet {
struct EntityRuntimeLimits {
    std::size_t entities = 4096;
    std::size_t requests = 1024;
    std::size_t queued_bytes = 16 * 1024 * 1024;
    std::size_t resident_bytes = 32 * 1024 * 1024;
    std::size_t receipts = 64;
    std::size_t receipt_bytes = 4 * 1024 * 1024;
    std::size_t delivery_messages = 64, delivery_channels = 256, delivery_sources = 4096;
    std::size_t delivery_bytes = 2 * 1024 * 1024;
    std::size_t state_bytes = 1024 * 1024;
    std::size_t timers=64,timer_bytes=256*1024;
    std::chrono::milliseconds lease{60000};
    std::chrono::milliseconds request_timeout{30000};
    std::chrono::milliseconds tick_interval{100};
    std::chrono::milliseconds idle_timeout{300000};
};
struct EntityHandle {
    std::string type, id, owner;
    std::uint64_t generation = 0;
};
struct EntityRuntimeStats {
    std::size_t entities = 0, pending = 0, queued_bytes = 0, state_bytes = 0, transient_bytes = 0;
    std::uint64_t publication_failures = 0;
    std::uint64_t committed = 0, replayed = 0, rejected = 0, failed = 0, tick_failures = 0;
};
// Stateful script-host decorator. Each operation restores opaque state, invokes
// script logic, CAS-replaces its single ownership + state record, then publishes its reply.
// All public methods and callbacks run on the owner thread. The parent outlives
// this host. owner_id must be globally unique per process incarnation.
class EntityScriptHost final : public ScriptHost {
public:
    using WallClock = std::function<std::int64_t()>; // epoch milliseconds; for deterministic tests
    EntityScriptHost(std::shared_ptr<ScriptHost> parent, std::shared_ptr<Storage>,
                     std::string owner_id, EntityRuntimeLimits = {}, StorageLimits = {}, WallClock = {});
    ~EntityScriptHost() override;
    bool has_route(const std::string&) const override;
    void bind(std::string, Handler) override;
    void define_schema(std::string, EntitySchema) override;
    void create_entity(std::string type,EntityAttributes::Values,EntitySpawnOptions,Reply) override;
    void bind_entity_owner(std::string type,std::string id,ConnectionRef,Reply) override;
    std::string schedule_timer(const std::string&,const std::string&,EntityTimerSpec)override;
    std::string timer_key(const std::string&,const std::string&)const override;
    bool cancel_timer(const std::string&,const std::string&,const std::string&)override;
    std::optional<EntityTimer> entity_timer(const std::string&,const std::string&,const std::string&)const override;
    void fire_timer(std::string type,std::string id,std::string key,std::string generation,std::int64_t due_ms,Reply);
    std::uint64_t timer_revision()const; // owner-thread discovery wake hint, not persisted
    std::int64_t now_ms()const;
    using CreateOnNode=std::function<void(std::string type,std::string id,std::string target,Reply)>;
    void on_create_on_node(CreateOnNode);
    void enable_global_entities();
    using Visibility = std::function<void(std::uint64_t,std::string,std::string,unsigned,Reply)>;
    void on_visibility(Visibility);
    void entity_visibility(std::uint64_t,std::string,std::string,unsigned,Reply) override;
    void forget_schema(const std::string&) override;
    struct AttributeCommit { std::string type, id, owner; std::shared_ptr<const EntitySchema> schema; AttributeUpdate previous, current; };
    using AttributeListener = std::function<void(const AttributeCommit&)>;
    // Runs only after a successful fenced storage CAS, on the owner thread.
    // A throwing listener cannot roll back an already committed entity.
    void on_attributes(AttributeListener);
    using Closed = std::function<void(const std::string&,const std::string&,bool destroyed)>;
    // Also invalidates projections after an uncertain storage commit. The
    // entity's handle generation changes; clients must acquire a new snapshot.
    void on_closed(Closed);
    std::shared_ptr<const EntitySchema> schema(const std::string& type) const;
    std::string owner_binding(const std::string& type, const std::string& id) const;
    std::string entity_owner(const std::string& type, const std::string& id) const override { return owner_binding(type,id); }
    void write_attributes(std::string type, std::string id, CallIdentity, std::string proposal, Reply, std::string request_id = {});
    // Trusted framework API, not a public RPC: serialized with entity operations,
    // returns the full image for subsequent recipient-specific filtering.
    void read_attributes(std::string type, std::string id, Reply);
    using AttributeGuard = std::function<bool(const std::string& owner_key)>;
    // Reads only metadata without acquiring a lease or invoking lifecycle hooks.
    void read_owner_binding(std::string type, std::string id, Reply);
    // Authorizes before lease acquisition, checks the same owner again in the
    // worker, and rechecks the guard before lifecycle execution and publication.
    void read_attributes(std::string type, std::string id, AttributeGuard, Reply);
    void bind_context(std::string, RpcPolicy, ContextHandler) override;
    void bind_stateful_context(std::string, std::string, std::string, RpcPolicy, ContextHandler) override;
    // Updates the shared method policy, rechecked by queued and resumed calls.
    void rpc_policy(const std::string&, RpcPolicy) override;
    void unbind(const std::string&) override;
    void call_entity(std::string, std::string, std::chrono::milliseconds, Reply) override;
    std::uint64_t connect(const std::string&, std::uint16_t) override;
    void call(std::uint64_t, std::string, std::string, std::chrono::milliseconds, Reply) override;
    std::size_t poll(std::size_t budget = 64);
    bool active(const std::string& type,const std::string& id) const;
    std::optional<EntityHandle> handle(const std::string& type, const std::string& id) const;
    // Only an idle entity can deactivate. Releases its lease with a fenced CAS;
    // already-committed state remains durable. Stale local handles are rejected.
    void deactivate(const EntityHandle&, Reply);
    // Trusted handoff primitive: idle mailbox, lifecycle hook and final snapshot
    // commit atomically with a durable target reservation. Reply contains an 8-byte transfer ID.
    void handoff(const EntityHandle&, std::string target, Reply);
    void accept_handoff(std::string type,std::string id,std::uint64_t transfer_id,Reply);
    bool accepts_handoff(const std::string& type,const std::string& id) const;
    const std::string& owner_id() const;
    using Transfer = std::function<void(std::string,std::string,std::string,Reply)>;
    void on_transfer(Transfer);
    void enable_delivery(bool);
    void send_entity(std::string source_type,std::string source_id,std::string type,std::string id,std::string method,std::string payload,Reply) override;
    std::optional<DeliveryMessage> delivery_status(const std::string&,const std::string&,const std::string&) const override;
    bool forget_delivery(const std::string&,const std::string&,const std::string&) override;
    // Only called by the authenticated delivery adapter; serialized in entity mailboxes.
    void receive_delivery(CallIdentity,DeliveryEnvelope,Reply);
    void acknowledge_delivery(std::string type,std::string id,std::uint64_t epoch,std::string ticket,Result,Reply);
    void transfer_entity(std::string,std::string,std::string,Reply) override;
    void control_entity(std::string type,std::string id,bool destroy,Reply) override;
    // Drains resident idle entities through on_deactivate before callers stop Lua.
    void drain(Reply);
    EntityRuntimeStats stats() const;
    void stop();
private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};
}

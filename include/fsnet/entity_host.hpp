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
    std::size_t state_bytes = 1024 * 1024;
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
    EntityScriptHost(std::shared_ptr<ScriptHost> parent, std::shared_ptr<Database>,
                     std::string owner_id, EntityRuntimeLimits = {}, StorageLimits = {}, WallClock = {});
    ~EntityScriptHost() override;
    bool has_route(const std::string&) const override;
    void bind(std::string, Handler) override;
    void bind_stateful(std::string route, std::string type, std::string initial, Handler) override;
    void define_schema(std::string, EntitySchema) override;
    void spawn_entity(std::string type, std::string id, EntityAttributes::Values, Reply) override;
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
    void on_closed(Closed);
    std::shared_ptr<const EntitySchema> schema(const std::string& type) const;
    std::string owner_subject(const std::string& type, const std::string& id) const;
    std::string entity_owner(const std::string& type, const std::string& id) const override { return owner_subject(type,id); }
    void write_attributes(std::string type, std::string id, CallIdentity, std::string proposal, Reply, std::string request_id = {});
    // Trusted framework API, not a public RPC: serialized with entity operations,
    // returns the full image for subsequent recipient-specific filtering.
    void read_attributes(std::string type, std::string id, Reply);
    void bind_context(std::string, RpcPolicy, ContextHandler) override;
    void bind_stateful_context(std::string, std::string, std::string, RpcPolicy, ContextHandler) override;
    using OwnerResolver = std::function<std::string(const std::string& type, const std::string& id)>;
    // Trusted application mapping from entity identity to account subject. Empty
    // results deny owner-only calls; the default resolver denies all such calls.
    void ownership(OwnerResolver);
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

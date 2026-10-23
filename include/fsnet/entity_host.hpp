#pragma once
#include "fsnet/async_database.hpp"
#include "fsnet/script.hpp"
#include <optional>

namespace fsnet {
struct EntityRuntimeLimits {
    std::size_t entities = 4096;
    std::size_t requests = 1024;
    std::size_t queued_bytes = 16 * 1024 * 1024;
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
    std::size_t entities = 0, pending = 0, queued_bytes = 0;
    std::uint64_t committed = 0, rejected = 0, failed = 0, tick_failures = 0;
};
// Stateful script-host decorator. Each operation restores opaque state, invokes
// script logic, atomically fences + saves the next state, then publishes its reply.
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
    void unbind(const std::string&) override;
    std::uint64_t connect(const std::string&, std::uint16_t) override;
    void call(std::uint64_t, std::string, std::string, std::chrono::milliseconds, Reply) override;
    std::size_t poll(std::size_t budget = 64);
    std::optional<EntityHandle> handle(const std::string& type, const std::string& id) const;
    // Only an idle entity can deactivate. Releases its lease with a fenced CAS;
    // already-committed state remains durable. Stale local handles are rejected.
    void deactivate(const EntityHandle&, Reply);
    EntityRuntimeStats stats() const;
    void stop();
private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};
}

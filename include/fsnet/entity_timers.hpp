#pragma once
#include "fsnet/entity_host.hpp"

namespace fsnet {
struct EntityTimerLimits {
    std::size_t pending=16,tracked=65536;
    std::chrono::milliseconds refresh{100},retry{100};
};
struct EntityTimerStats {std::size_t tracked=0,pending=0;std::uint64_t fired=0,failures=0,capacity_failures=0;};
// Run on every owner, alongside host/VM polling. Durable discovery survives a
// stopped/idle host and process restart; the host fences and serializes callbacks.
class EntityTimers {
public:
    EntityTimers(std::shared_ptr<EntityScriptHost>,std::shared_ptr<Database>,EntityTimerLimits={});
    ~EntityTimers();
    EntityTimers(const EntityTimers&)=delete;
    EntityTimers& operator=(const EntityTimers&)=delete;
    std::size_t poll(std::size_t budget=64);
    EntityTimerStats stats()const;
    void stop();
private:
    struct Impl;std::shared_ptr<Impl> impl_;
};
}

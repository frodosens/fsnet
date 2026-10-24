#pragma once
#include "fsnet/entity_host.hpp"
#include "fsnet/entity_router.hpp"
namespace fsnet {
struct EntityDeliveryLimits {
    std::size_t requests=32;
    // retry is the pause between complete registry sweeps; bounded chunks of
    // the same sweep continue as soon as async capacity becomes available.
    std::chrono::milliseconds retry{100},refresh{1000},timeout{3000};
};
struct EntityDeliveryStats {
    std::size_t sources=0,pending=0;
    std::uint64_t attempts=0,acknowledged=0,failures=0;
};
// Every owner may run a worker over the same durable source registry. Duplicated
// attempts are safe; per-source FIFO plus receiver watermarks prevent reexecution.
// Pending work has no expiry. All resources and VM callbacks use the owner thread.
class EntityDelivery {
public:
    using Trust=std::function<bool(const CallIdentity&)>;
    EntityDelivery(Node&,std::shared_ptr<EntityScriptHost>,std::shared_ptr<Database>,
                   std::vector<EntityEndpoint>,Trust,EntityDeliveryLimits = {});
    ~EntityDelivery();
    EntityDelivery(const EntityDelivery&)=delete;
    EntityDelivery& operator=(const EntityDelivery&)=delete;
    void poll(std::size_t budget=64);
    EntityDeliveryStats stats() const;
    void stop();
private:
    struct Impl;std::shared_ptr<Impl> impl_;
};
}

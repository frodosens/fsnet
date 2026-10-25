#pragma once
#include "fsnet/entity_router.hpp"
#include "fsnet/entity_host.hpp"

namespace fsnet {
struct EntityTransferLimits {
    std::size_t requests=64;
    std::chrono::milliseconds timeout{10000}; // each network phase
};
// Install on each owner. Node identity must authenticate owner certificates;
// delegated client connections never qualify. Poll alongside Node/host/VM.
// A timeout after release has an unknown outcome: retry the SAME destination.
class EntityTransfer {
public:
    using Trust=std::function<bool(const CallIdentity&)>;
    EntityTransfer(Node&,std::shared_ptr<EntityScriptHost>,std::shared_ptr<Storage>,
                   std::vector<EntityEndpoint>,Trust,EntityTransferLimits = {});
    ~EntityTransfer();
    EntityTransfer(const EntityTransfer&)=delete;
    EntityTransfer& operator=(const EntityTransfer&)=delete;
    void transfer(std::string type,std::string id,std::string target,Reply);
    std::size_t poll(std::size_t budget=64);
    std::size_t pending() const;
    void stop();
private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};
}

#pragma once
#include "fsnet/entity_host.hpp"

namespace fsnet {
struct ReplicationLimits {
    std::size_t subscriptions = 1024, per_peer = 32, image_bytes = 16 * 1024 * 1024;
    std::chrono::milliseconds pending_timeout{5000}, lease{30000};
};
// Local owner-node replication service. Its node/host must outlive it. Observer
// authorization is a trusted scene/AOI hook, evaluated on subscription and updates.
// The host's sole post-commit listener is owned by this service while it runs.
class AttributeReplication {
public:
    using Observer = std::function<bool(const CallIdentity&, const std::string& type, const std::string& id)>;
    AttributeReplication(Node&, std::shared_ptr<EntityScriptHost>, Observer = {}, ReplicationLimits = {});
    ~AttributeReplication();
    // Trusted server scene API: 1 show, 2 hide, 3 possess, 4 unpossess.
    void visibility(std::uint64_t peer,std::string type,std::string id,unsigned action,Reply);
    std::size_t poll(std::size_t budget = 64);
    void stop();
private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};
// One client projection manager per Node. The expected server peer must be an
// authenticated connection (TLS outside loopback). Poll renews bounded leases.
struct AttributeProjectionInfo {
    std::uint64_t subscription;
    std::string type,id;
    bool automatic,owned,controlled;
};
class AttributeClient {
public:
    using Ready = std::function<void(Status, std::uint64_t subscription)>;
    using Written = std::function<void(Status)>;
    using Changed = std::function<void(std::uint64_t subscription, Status, const AttributeReplica*)>;
    using Presence = std::function<void(const EntityPresence&)>;
    AttributeClient(Node&, std::uint64_t server_peer, Changed = {}, Presence = {});
    ~AttributeClient();
    void watch(std::string type, std::string id, Ready, std::chrono::milliseconds timeout = std::chrono::seconds(3));
    void write(std::uint64_t subscription, EntityAttributes::Values, Written,
               AttributeWriteOptions = {}, std::chrono::milliseconds timeout = std::chrono::seconds(3));
    void unwatch(std::uint64_t subscription);
    void stop_entities(); // release auto projections; retain ordinary manual watches
    const AttributeReplica* get(std::uint64_t subscription) const;
    std::optional<AttributeProjectionInfo> projection_info(std::uint64_t) const;
    std::vector<AttributeProjectionInfo> projection_infos() const;
    void poll();
    void stop();
private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};
}

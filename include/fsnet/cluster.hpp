#pragma once
#include "fsnet/node.hpp"
#include <memory>
#include <vector>

namespace fsnet {
struct Endpoint {
    std::string address;
    std::uint16_t port;
    std::vector<std::string> services;
};
// A static-membership client mesh. The Node must outlive this object. Call tick
// on the owning Node thread. No leader or singleton center is required.
class ClusterClient {
public:
    ClusterClient(Node& node, std::vector<Endpoint> endpoints);
    ~ClusterClient();
    ClusterClient(const ClusterClient&) = delete;
    ClusterClient& operator=(const ClusterClient&) = delete;
    void tick();
    bool available(const std::string& service, const std::string& entity) const;
    void call(std::string service, std::string entity, std::string route,
              std::string data, std::chrono::milliseconds timeout, Reply reply,
              bool idempotent = false);
private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};
}

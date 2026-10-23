#include "fsnet/cluster.hpp"
#include "fsnet/directory.hpp"
#include <stdexcept>

namespace fsnet {
namespace { using Clock = std::chrono::steady_clock; using namespace std::chrono_literals; }
struct ClusterClient::Impl : std::enable_shared_from_this<ClusterClient::Impl> {
    struct Peer {
        Endpoint endpoint;
        std::uint64_t connection = 0;
        bool probing = false;
        unsigned failures = 0;
        Clock::time_point next{};
    };
    Node& node;
    std::vector<Peer> peers;
    ServiceDirectory directory;
    bool closing = false;
    explicit Impl(Node& owner) : node(owner) {}
    void failed(std::size_t index, std::uint64_t connection) {
        auto& peer = peers[index];
        if (peer.connection != connection) return;
        directory.remove(index + 1);
        peer.probing = false;
        peer.connection = 0;
        peer.failures = std::min(peer.failures + 1, 6U);
        peer.next = Clock::now() + 100ms * (1U << peer.failures);
        node.close(connection);
    }
    void attempt(std::string service, std::string entity, std::string route, std::string data,
                 Clock::time_point deadline, Reply reply, bool retry) {
        auto slot = directory.locate(service, entity);
        if (!slot || closing) return reply({Status::disconnected, {}});
        auto index = static_cast<std::size_t>(*slot - 1);
        auto connection = peers[index].connection;
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
        if (remaining <= 0ms) return reply({Status::timeout, {}});
        node.call(connection, route, data, remaining,
          [weak = weak_from_this(), index, connection, service = std::move(service), entity = std::move(entity),
           route, data, deadline, reply = std::move(reply), retry](Result result) mutable {
            auto self = weak.lock();
            if (!self) return reply({Status::disconnected, {}});
            bool transport_failure = result.status == Status::disconnected || result.status == Status::timeout;
            if (transport_failure) self->failed(index, connection);
            if (retry && transport_failure && !self->closing && self->directory.locate(service, entity))
                return self->attempt(std::move(service), std::move(entity), std::move(route), std::move(data), deadline, std::move(reply), false);
            reply(std::move(result));
          });
    }
};
ClusterClient::ClusterClient(Node& node, std::vector<Endpoint> endpoints) : impl_(std::make_shared<Impl>(node)) {
    if (endpoints.empty() || endpoints.size() > 1024) throw std::invalid_argument("invalid cluster membership");
    for (auto& endpoint : endpoints) {
        if (!endpoint.port || endpoint.services.empty()) throw std::invalid_argument("invalid cluster endpoint");
        impl_->peers.push_back({std::move(endpoint)});
    }
}
ClusterClient::~ClusterClient() {
    impl_->closing = true;
    for (auto& peer : impl_->peers) if (peer.connection) impl_->node.close(peer.connection);
}
void ClusterClient::tick() {
    auto now = Clock::now();
    for (std::size_t index = 0; index < impl_->peers.size(); ++index) {
        auto& peer = impl_->peers[index];
        if (peer.probing || peer.next > now) continue;
        if (!peer.connection) {
            try { peer.connection = impl_->node.connect(peer.endpoint.address, peer.endpoint.port); }
            catch (...) { peer.next = now + 1s; continue; }
        }
        auto connection = peer.connection;
        peer.probing = true;
        impl_->node.call(connection, "system.health", {}, 500ms,
          [weak = std::weak_ptr(impl_), index, connection](Result result) {
            auto self = weak.lock();
            if (!self || self->closing) return;
            auto& current = self->peers[index];
            if (current.connection != connection) return;
            if (result.status != Status::ok || result.data != "ready") return self->failed(index, connection);
            current.probing = false; current.failures = 0;
            current.next = Clock::now() + 1s;
            for (auto& service : current.endpoint.services) self->directory.renew(service, index + 1, 2s);
          });
    }
}
bool ClusterClient::available(const std::string& service, const std::string& entity) const {
    return impl_->directory.locate(service, entity).has_value();
}
void ClusterClient::call(std::string service, std::string entity, std::string route,
                         std::string data, std::chrono::milliseconds timeout, Reply reply, bool idempotent) {
    if (!reply || timeout.count() <= 0) throw std::invalid_argument("invalid cluster RPC");
    impl_->attempt(std::move(service), std::move(entity), std::move(route), std::move(data), Clock::now() + timeout, std::move(reply), idempotent);
}
}

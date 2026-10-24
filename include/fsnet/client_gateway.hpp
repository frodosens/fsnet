#pragma once
#include "fsnet/entity_router.hpp"
#include "fsnet/entity_host.hpp"
namespace fsnet {
// Owns Node's identity resolver. A dedicated authenticated gateway connection
// becomes one client principal; role is always client, never payload-controlled.
class GatewayDelegation {
public:
    using Trust = std::function<bool(const CallIdentity&,const std::string& certificate)>;
    using Restore = std::function<bool(const CallIdentity&,const std::string&,const std::string&,bool controlled)>;
    GatewayDelegation(Node&,Node::IdentityResolver transport_identity,Trust,std::shared_ptr<EntityScriptHost>,Restore = {},std::size_t sessions = 1024);
    ~GatewayDelegation();
    void poll();
    void stop();
private:
    struct Impl; std::shared_ptr<Impl> impl_;
};
struct GatewayLimits {
    std::size_t clients=256, links=1024, subscriptions=1024, per_client=32, requests=256, bytes=4*1024*1024;
    std::chrono::milliseconds request_timeout{3000}, refresh{100}, migration_timeout{90000};
};
struct GatewayStats { std::size_t clients=0,links=0,subscriptions=0,pending=0,queued_bytes=0,migrating=0; std::uint64_t forwarded=0,failures=0,migrations=0; };
// Front and back Nodes are separate so client notifications cannot inject owner
// traffic. Client authentication belongs to front.identify; owner hops use back's
// TLS identity and the explicit delegation gate above.
class ClientGateway {
public:
    ClientGateway(Node& front,Node& back,std::shared_ptr<Database>,std::vector<EntityEndpoint>,GatewayLimits = {});
    ~ClientGateway();
    void bind_entity(std::string type,std::vector<std::string> methods);
    // Stateless login/scene endpoints route by authenticated account subject.
    void bind_account(std::string route,std::string entity_type);
    void poll(std::size_t budget=64);
    GatewayStats stats() const;
    void drain(); // reject new requests; poll accepted requests before stop
    void stop();
private:
    struct Impl; std::shared_ptr<Impl> impl_;
};
}

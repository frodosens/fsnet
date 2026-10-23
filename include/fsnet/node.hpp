#pragma once
#include "fsnet/entity_schema.hpp"
#include <chrono>
#include <optional>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace fsnet {
enum class Status : std::uint8_t { ok, not_found, overloaded, timeout, disconnected, rejected, internal };
struct Result { Status status; std::string data; };
using Reply = std::function<void(Result)>;
using Handler = std::function<void(std::string, Reply)>;
struct TlsConfig {
    std::string ca_file, certificate_file, private_key_file;
    std::string peer_name; // outbound certificate name; default is the target IP
};
struct Limits {
    std::size_t frame_bytes = 1024 * 1024;
    std::size_t queued_bytes = 4 * 1024 * 1024;
    std::size_t connections = 1024;
    std::size_t pending_calls = 4096;
    std::size_t inflight_requests = 128;
    std::chrono::milliseconds idle_timeout{30000};
};
// All Node methods/callbacks run on its owning thread. Separate nodes can run on
// separate threads/processes. No user code runs on a networking worker thread.
class Node {
public:
    explicit Node(std::string token, Limits limits = {}, TlsConfig tls = {});
    ~Node();
    Node(const Node&) = delete;
    Node& operator=(const Node&) = delete;
    std::uint16_t listen(const std::string& address, std::uint16_t port);
    std::uint64_t connect(const std::string& address, std::uint16_t port, const std::string& peer_name = {});
    using Authorizer = std::function<bool(std::uint64_t, const std::string&, const std::string&)>;
    // Called after transport + token authentication, before dispatch. Identity is
    // the verified certificate SHA-256 fingerprint, or empty on loopback plaintext.
    void authorize(Authorizer);
    std::string peer_identity(std::uint64_t peer) const;
    using IdentityResolver = std::function<std::optional<CallIdentity>(std::uint64_t, const std::string&)>;
    using ContextHandler = std::function<void(const CallIdentity&, std::string, Reply)>;
    // Trusted application hook. Input identity is the verified TLS fingerprint;
    // never derive a server role from a field supplied in an RPC payload.
    // With no resolver, connections are clients identified by their certificate.
    void identify(IdentityResolver);
    void bind_rpc(std::string route, RpcPolicy, ContextHandler);
    void rpc_policy(const std::string& route, RpcPolicy);
    // Legacy binary host API; migration to explicit policies is still required.
    void bind(std::string route, Handler handler);
    bool has_route(const std::string& route) const;
    bool unbind(const std::string& route);
    void call(std::uint64_t peer, std::string route, std::string data,
              std::chrono::milliseconds timeout, Reply reply);
    void close(std::uint64_t peer);
    void poll();
    void run();
    void stop();
    std::size_t pending() const;
private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};
}

#pragma once
#include "fsnet/attribute_wire.hpp"
#include "fsnet/node.hpp"
#include "fsnet/entity_id.hpp"
#include <optional>
#include <vector>
namespace fsnet {
class ScriptHost;
struct ClientOptions {
    std::string token;
    TlsConfig tls;
    Limits transport;
    std::chrono::milliseconds connect_timeout{5000},request_timeout{3000};
};
struct ClientEndpoint { std::string address; std::uint16_t port=0; std::string server_name; };
struct EntityKey { std::string type,id; bool operator==(const EntityKey&) const = default; };
// Generation prevents a handle from an old connection addressing a new replica.
struct ClientEntityHandle {
    std::uint64_t generation=0,subscription=0;
    explicit operator bool() const { return generation && subscription; }
    bool operator==(const ClientEntityHandle&) const = default;
};
struct ClientEntity {
    EntityKey key;
    ClientEntityHandle handle;
    std::shared_ptr<const AttributeUpdate> confirmed;
    bool automatic=false;
    std::optional<bool> owned; // available on server-created presence, not manual watches
    bool controlled=false;
};
enum class ClientState { disconnected,connecting,connected };
struct ClientEvent {
    enum class Kind { connecting,connected,disconnected,enter,changed,control,leave } kind;
    std::uint64_t generation=0;
    Status status=Status::ok;
    EntityKey key;
    ClientEntityHandle handle;
    bool automatic=false;
    std::optional<bool> owned;
    bool controlled=false;
};
struct ClientRequestOptions { std::string request_id; std::chrono::milliseconds timeout{0}; };
struct ClientStats { ClientState state; std::uint64_t generation; std::size_t entities; TransportStats transport; };
// Owner-thread, poll-driven SDK. Uses the common Node, AttributeClient, codecs
// and immutable snapshots. No gameplay, login policy or automatic RPC retries.
class Client {
public:
    using Events=std::function<void(const ClientEvent&)>;
    using Ready=std::function<void(Status,ClientEntityHandle)>;
    explicit Client(ClientOptions);
    ~Client();
    Client(const Client&)=delete;
    Client& operator=(const Client&)=delete;
    void on_event(Events);
    // Numeric IP endpoint; server_name controls TLS verification, not DNS lookup.
    // Reconnect explicitly after disconnect; requests are never silently replayed.
    void connect(ClientEndpoint);
    void disconnect();
    void poll();
    ClientState state() const;
    ClientStats stats() const;
    std::string last_error() const;
    void request(std::string_view route,std::string_view payload,Reply,
                 std::chrono::milliseconds timeout=std::chrono::milliseconds{0});
    void call(const EntityKey&,std::string_view method,std::string_view payload,Reply,ClientRequestOptions = {});
    void call(EntityId,std::string_view method,std::string_view payload,Reply,ClientRequestOptions = {});
    void watch(EntityKey,Ready,std::chrono::milliseconds timeout=std::chrono::milliseconds{0});
    void write(ClientEntityHandle,EntityAttributes::Values,Reply,AttributeWriteOptions = {},
               std::chrono::milliseconds timeout=std::chrono::milliseconds{0});
    void unwatch(ClientEntityHandle);
    std::optional<ClientEntity> entity(ClientEntityHandle) const;
    // Prefer an automatic projection, preserving ownership/control metadata.
    // With duplicate watches of one kind, choose the oldest live subscription.
    std::optional<ClientEntity> entity(const EntityKey&) const;
    std::optional<ClientEntity> entity(EntityId) const;
    std::optional<ClientEntity> controlled() const;
    std::vector<ClientEntity> entities() const;
    // One script consumer per connection. Acquire outside poll, then construct
    // LuaEngine with this host; poll Client and engine on the same owner thread.
    // After reconnect, stop the old engine and acquire a new host.
    std::shared_ptr<ScriptHost> script_host();
private:
    struct Impl; std::shared_ptr<Impl> impl_;
};
}

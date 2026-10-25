#include "fsnet/node.hpp"
#include <asio.hpp>
#include <asio/ssl.hpp>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <array>
#include <deque>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fsnet {
using Tcp = asio::ip::tcp;
namespace {
void put(std::string& out, std::uint64_t value, unsigned bytes) {
    for (unsigned i = bytes; i; --i) out.push_back(static_cast<char>(value >> ((i - 1) * 8)));
}
std::uint64_t get(const char* in, unsigned bytes) {
    std::uint64_t value = 0;
    for (unsigned i = 0; i < bytes; ++i) value = (value << 8) | static_cast<unsigned char>(in[i]);
    return value;
}
std::string frame(unsigned kind, Status status, std::uint64_t id, std::string_view route, std::string_view data) {
    std::string out;
    out.reserve(18 + route.size() + data.size());
    put(out, 14 + route.size() + data.size(), 4);
    put(out, 1, 1); put(out, kind, 1); put(out, static_cast<unsigned>(status), 1); put(out, 0, 1);
    put(out, id, 8); put(out, route.size(), 2);
    out += route; out += data;
    return out;
}
bool equal_token(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    unsigned diff = 0;
    for (std::size_t i = 0; i < a.size(); ++i) diff |= static_cast<unsigned char>(a[i] ^ b[i]);
    return diff == 0;
}
}
struct Node::Impl : std::enable_shared_from_this<Node::Impl> {
    struct Session;
    struct Pending {
        std::uint64_t peer;
        std::shared_ptr<asio::steady_timer> timer;
        Reply reply;
    };
    asio::io_context io;
    Tcp::acceptor acceptor{io};
    std::string token;
    Limits limits;
    TlsConfig tls_config;
    std::unique_ptr<asio::ssl::context> tls;
    Authorizer authorizer;
    IdentityResolver identity_resolver;
    std::unordered_map<std::string, Notification> notifications;
    struct Route { RpcPolicy policy; ContextHandler handler; bool legacy = false; };
    bool stopped = false, ready = true, driving = false;
    std::size_t receive_bytes = 0;
    std::size_t callback_failures = 0;
    std::uint64_t peer_serial = 0, call_serial = 0;
    std::unordered_map<std::uint64_t, std::shared_ptr<Session>> sessions;
    std::unordered_map<std::uint64_t, Pending> calls;
    std::unordered_map<std::string, Route> handlers;
    Impl(std::string auth, Limits settings, TlsConfig security) : token(std::move(auth)), limits(settings), tls_config(std::move(security)) {
        if ((token.empty() && tls_config.ca_file.empty()) || token.size() > 4096 || limits.frame_bytes < token.size() + 14 ||
            limits.frame_bytes > 16 * 1024 * 1024 || limits.queued_bytes < limits.frame_bytes + 4 ||
            !limits.connections || !limits.pending_calls || !limits.inflight_requests || limits.idle_timeout.count() <= 0 ||
            limits.receive_bytes < limits.frame_bytes)
            throw std::invalid_argument("invalid token or node limits");
        if (!tls_config.ca_file.empty() || !tls_config.certificate_file.empty() || !tls_config.private_key_file.empty()) {
            if (tls_config.ca_file.empty() || tls_config.certificate_file.empty() || tls_config.private_key_file.empty())
                throw std::invalid_argument("TLS requires CA, certificate and private key");
            tls = std::make_unique<asio::ssl::context>(asio::ssl::context::tls);
            tls->load_verify_file(tls_config.ca_file);
            tls->use_certificate_chain_file(tls_config.certificate_file);
            tls->use_private_key_file(tls_config.private_key_file, asio::ssl::context::pem);
            if (!SSL_CTX_check_private_key(tls->native_handle()) || !SSL_CTX_set_min_proto_version(tls->native_handle(), TLS1_3_VERSION))
                throw std::runtime_error("TLS configuration failed");
            tls->set_verify_mode(asio::ssl::verify_peer | asio::ssl::verify_fail_if_no_peer_cert);
            SSL_CTX_set_max_early_data(tls->native_handle(), 0);
            SSL_CTX_set_options(tls->native_handle(), SSL_OP_NO_COMPRESSION);
        } else if (!tls_config.peer_name.empty()) throw std::invalid_argument("peer name requires TLS");
    }
    void accept();
    void drain() {
        // Called only outside a running io_context operation. restart() while
        // run/poll is still on the stack is not supported by Asio.
        io.restart(); io.poll(); io.stop();
    }
    void drive(bool blocking) {
        if (stopped) return;
        if (driving) throw std::logic_error("recursive node poll/run");
        driving = true;
        try { io.restart(); if (blocking) io.run(); else io.poll(); }
        catch (...) { driving = false; if (stopped) drain(); throw; }
        driving = false;
        if (stopped) drain();
    }
    void finish(std::uint64_t id, Result result) {
        auto it = calls.find(id);
        if (it == calls.end()) return;
        auto pending = std::move(it->second);
        calls.erase(it);
        pending.timer->cancel();
        deliver(std::move(pending.reply),std::move(result));
    }
    void deliver(Reply reply,Result result) noexcept {
        try { reply(std::move(result)); } catch(...) { ++callback_failures; }
    }
    void lost(std::uint64_t peer) {
        sessions.erase(peer);
        std::vector<std::uint64_t> failed;
        for (auto& [id, pending] : calls) if (pending.peer == peer) failed.push_back(id);
        for (auto id : failed) finish(id, {Status::disconnected, {}});
    }
};
struct Node::Impl::Session : std::enable_shared_from_this<Session> {
    std::weak_ptr<Impl> owner;
    Tcp::socket socket;
    std::unique_ptr<asio::ssl::stream<Tcp::socket&>> tls;
    std::string identity;
    asio::steady_timer idle;
    std::uint64_t id;
    bool closed = false, active = false, authenticated = false;
    std::size_t queued = 0, inflight = 0;
    std::array<char, 4> header{};
    std::string body;
    std::size_t receive_reserved = 0;
    std::deque<std::shared_ptr<std::string>> output;
    Session(std::shared_ptr<Impl> node, std::uint64_t serial)
      : owner(node), socket(node->io), idle(node->io), id(serial) {
        if (node->tls) tls = std::make_unique<asio::ssl::stream<Tcp::socket&>>(socket, *node->tls);
        enqueue(frame(0, Status::ok, 0, {}, node->token));
    }
    ~Session() { release_body(); }
    void release_body() {
        if (auto node = owner.lock()) node->receive_bytes -= receive_reserved;
        receive_reserved = 0;
        std::string{}.swap(body);
    }
    void close() {
        if (std::exchange(closed, true)) return;
        asio::error_code ignored;
        idle.cancel(); socket.close(ignored);
        output.clear(); queued = 0;
        if (auto node = owner.lock()) node->lost(id);
    }
    void deadline() {
        auto node = owner.lock();
        if (!node || closed) return;
        idle.expires_after(node->limits.idle_timeout);
        idle.async_wait([self = shared_from_this()](asio::error_code error) { if (!error) self->close(); });
    }
    bool enqueue(std::string message) {
        auto node = owner.lock();
        if (!node || closed || message.size() > node->limits.queued_bytes - queued) return false;
        bool writing = !output.empty();
        queued += message.size();
        output.push_back(std::make_shared<std::string>(std::move(message)));
        if (!writing && active) write();
        return true;
    }
    template<class Buffer, class Callback> void read(Buffer buffer, Callback callback) {
        if (tls) asio::async_read(*tls, buffer, std::move(callback));
        else asio::async_read(socket, buffer, std::move(callback));
    }
    template<class Buffer, class Callback> void send(Buffer buffer, Callback callback) {
        if (tls) asio::async_write(*tls, buffer, std::move(callback));
        else asio::async_write(socket, buffer, std::move(callback));
    }
    void ready() {
        if (closed) return;
        if (tls) {
            if (SSL_get_verify_result(tls->native_handle()) != X509_V_OK) return close();
            auto* certificate = SSL_get0_peer_certificate(tls->native_handle());
            if (!certificate) return close();
            unsigned char digest[EVP_MAX_MD_SIZE]; unsigned length = 0;
            if (!X509_digest(certificate, EVP_sha256(), digest, &length)) return close();
            identity = "sha256:";
            constexpr char hex[] = "0123456789abcdef";
            for (unsigned i = 0; i < length; ++i) { identity += hex[digest[i] >> 4]; identity += hex[digest[i] & 15]; }
        }
        active = true; deadline(); if (!output.empty()) write(); read_header();
    }
    void start(bool server = false) {
        asio::error_code error;
        socket.set_option(Tcp::no_delay(true), error);
        if (error) return close();
        deadline();
        if (tls) tls->async_handshake(server ? asio::ssl::stream_base::server : asio::ssl::stream_base::client,
            [self = shared_from_this()](asio::error_code error) { if (error) self->close(); else self->ready(); });
        else ready();
    }
    void write() {
        if (closed || output.empty()) return;
        auto message = output.front();
        send(asio::buffer(*message),
          [self = shared_from_this(), message](asio::error_code error, std::size_t) {
            if (self->closed) return;
            if (error) return self->close();
            self->queued -= message->size(); self->output.pop_front(); self->write();
          });
    }
    void read_header() {
        read(asio::buffer(header),
          [self = shared_from_this()](asio::error_code error, std::size_t) {
            if (self->closed) return;
            auto node = self->owner.lock();
            auto size = get(self->header.data(), 4);
            if (error || !node || size < 14 || size > node->limits.frame_bytes ||
                (!self->authenticated && size != 14 + node->token.size()) ||
                size > node->limits.receive_bytes - node->receive_bytes) return self->close();
            node->receive_bytes += size; self->receive_reserved = size;
            try { self->body.resize(size); }
            catch (...) { self->release_body(); self->close(); return; }
            self->read(asio::buffer(self->body),
              [self](asio::error_code read_error, std::size_t) {
                // A canceled read still owns its buffer until this handler.
                if (self->closed || read_error) { self->release_body(); return self->close(); }
                try { self->deadline(); self->receive(); }
                catch (...) { self->release_body(); self->close(); throw; }
                self->release_body();
                if (!self->closed) self->read_header();
              });
          });
    }
    void receive() {
        auto node = owner.lock();
        if (!node) return close();
        unsigned version = get(body.data(), 1), kind = get(body.data() + 1, 1), code = get(body.data() + 2, 1);
        auto call = get(body.data() + 4, 8), route_size = get(body.data() + 12, 2);
        if (version != 1 || body[3] != 0 || code > static_cast<unsigned>(Status::internal) ||
            route_size > 256 || route_size > body.size() - 14) return close();
        std::string route = body.substr(14, route_size), data = body.substr(14 + route_size);
        if (!authenticated) {
            if (kind != 0 || call || !route.empty() || code || !equal_token(data, node->token)) return close();
            authenticated = true;
            return;
        }
        if (kind == 2 && route.empty() && call) {
            auto it = node->calls.find(call);
            if (it != node->calls.end() && it->second.peer == id)
                node->finish(call, {static_cast<Status>(code), std::move(data)});
            return;
        }
        if (kind == 3) {
            if (call || code || route.empty() || route.find('\0') != std::string::npos) return close();
            if (node->authorizer) {
                try { auto authorize = node->authorizer; if (!authorize(id, identity, route)) return close(); }
                catch (...) { return close(); }
                if (closed || node->stopped) return;
            }
            auto found = node->notifications.find(route);
            if (found != node->notifications.end()) {
                auto callback = found->second;
                try { callback(id, identity, std::move(data)); } catch (...) { close(); }
            }
            return;
        }
        if (kind != 1 || !call || route.empty() || route.find('\0') != std::string::npos || code) return close();
        if (inflight >= node->limits.inflight_requests) {
            if (!enqueue(frame(2, Status::overloaded, call, {}, {}))) close();
            return;
        }
        if (node->authorizer) {
            bool allowed = false;
            auto authorize = node->authorizer;
            try { allowed = authorize(id, identity, route); } catch (...) {}
            if (!allowed) { if (!enqueue(frame(2, Status::rejected, call, {}, {}))) close(); return; }
            if (closed || node->stopped) return;
        }
        auto it = node->handlers.find(route);
        if (it == node->handlers.end()) {
            if (!enqueue(frame(2, Status::not_found, call, {}, {}))) close();
            return;
        }
        auto registered = it->second;
        CallIdentity principal{Caller::client, {}};
        if(registered.legacy || !node->identity_resolver) principal.subject=identity;
        if (!registered.legacy) {
            try {
                if (node->identity_resolver) {
                    auto resolve = node->identity_resolver;
                    auto resolved = resolve(id, identity);
                    if (!resolved) { if (!enqueue(frame(2, Status::rejected, call, {}, {}))) close(); return; }
                    principal = std::move(*resolved);
                }
                if (principal.subject.size() > 256 || (principal.caller != Caller::client && principal.caller != Caller::server)) {
                    if (!enqueue(frame(2, Status::rejected, call, {}, {}))) close();
                    return;
                }
                // Entity ownership is checked by the entity dispatcher; this
                // transport layer can enforce only the authenticated caller role.
                if (!(principal.caller == Caller::server ? registered.policy.server : registered.policy.client)) {
                    if (!enqueue(frame(2, Status::rejected, call, {}, {}))) close();
                    return;
                }
            } catch (...) { if (!enqueue(frame(2, Status::rejected, call, {}, {}))) close(); return; }
            if (closed || node->stopped) return;
        }
        principal.peer = id;
        ++inflight;
        auto replied = std::make_shared<bool>(false);
        auto reply = [weak = weak_from_this(), replied, call](Result result) {
            if (std::exchange(*replied, true)) return;
            auto self = weak.lock();
            if (!self || self->closed) return;
            --self->inflight;
            auto owner = self->owner.lock();
            if (!owner) return;
            if (result.data.size() > owner->limits.frame_bytes - 14) result = {Status::overloaded, {}};
            if (!self->enqueue(frame(2, result.status, call, {}, result.data))) self->close();
        };
        // Copy before dispatch: handlers may register another route and rehash.
        auto handler = std::move(registered.handler);
        try { handler(principal, std::move(data), reply); }
        catch (...) { reply({Status::internal, {}}); }
    }
};
void Node::Impl::accept() {
    if (stopped) return;
    auto session = std::make_shared<Session>(shared_from_this(), ++peer_serial);
    acceptor.async_accept(session->socket, [self = shared_from_this(), session](asio::error_code error) {
        if (self->stopped) return;
        if (!error && self->sessions.size() < self->limits.connections) {
            self->sessions.emplace(session->id, session); session->start(true);
        }
        self->accept();
    });
}
Node::Node(std::string token, Limits limits, TlsConfig tls) : impl_(std::make_shared<Impl>(std::move(token), limits, std::move(tls))) {
    bind("system.health", [weak=std::weak_ptr(impl_)](std::string, Reply reply) { auto self=weak.lock(); reply(self&&self->ready?Result{Status::ok,"ready"}:Result{Status::overloaded,"draining"}); });
}
Node::~Node() { stop(); }
std::uint16_t Node::listen(const std::string& address, std::uint16_t port) {
    if (impl_->stopped || impl_->acceptor.is_open()) throw std::logic_error("node already listening or stopped");
    Tcp::endpoint endpoint(asio::ip::make_address(address), port);
    if (!impl_->tls && !endpoint.address().is_loopback()) throw std::invalid_argument("public listeners require TLS");
    impl_->acceptor.open(endpoint.protocol());
    impl_->acceptor.set_option(Tcp::acceptor::reuse_address(true));
    impl_->acceptor.bind(endpoint); impl_->acceptor.listen(); impl_->accept();
    return impl_->acceptor.local_endpoint().port();
}
std::uint64_t Node::connect(const std::string& address, std::uint16_t port, const std::string& peer_name) {
    if (impl_->stopped || impl_->sessions.size() >= impl_->limits.connections) throw std::runtime_error("connection limit");
    Tcp::endpoint endpoint(asio::ip::make_address(address), port);
    if (!impl_->tls && !endpoint.address().is_loopback()) throw std::invalid_argument("public connections require TLS");
    auto session = std::make_shared<Impl::Session>(impl_, ++impl_->peer_serial);
    if (session->tls) {
        auto expected = !peer_name.empty() ? peer_name : !impl_->tls_config.peer_name.empty() ? impl_->tls_config.peer_name : address;
        if (expected.find('\0') != std::string::npos) throw std::invalid_argument("invalid TLS peer name");
        session->tls->set_verify_callback(asio::ssl::host_name_verification(expected));
        if (!SSL_set_tlsext_host_name(session->tls->native_handle(), expected.c_str())) throw std::runtime_error("TLS server name failed");
    } else if (!peer_name.empty()) throw std::invalid_argument("peer name requires TLS");
    impl_->sessions.emplace(session->id, session);
    session->deadline();
    session->socket.async_connect(endpoint, [session](asio::error_code error) {
        if (session->closed) return;
        if (error) session->close(); else session->start();
    });
    return session->id;
}
void Node::authorize(Authorizer authorize) { impl_->authorizer = std::move(authorize); }
std::string Node::peer_identity(std::uint64_t peer) const {
    auto it = impl_->sessions.find(peer);
    return it == impl_->sessions.end() || !it->second->authenticated ? std::string{} : it->second->identity;
}
void Node::identify(IdentityResolver resolve) { impl_->identity_resolver = std::move(resolve); }
void Node::bind_rpc(std::string route, RpcPolicy policy, ContextHandler handler) {
    if (route.empty() || route.size() > 256 || route.find('\0') != std::string::npos || !handler || (!policy.client && !policy.server))
        throw std::invalid_argument("RPC requires a valid route, handler and explicit callers");
    // A plain transport route has no entity owner to compare against. Reject this
    // policy rather than silently claiming owner authorization has been enforced.
    if (policy.owner_only) throw std::invalid_argument("owner policy requires an entity dispatcher");
    if (!impl_->handlers.emplace(std::move(route), Impl::Route{policy, std::move(handler), false}).second)
        throw std::logic_error("duplicate RPC route");
}
void Node::rpc_policy(const std::string& route, RpcPolicy policy) {
    if ((!policy.client && !policy.server) || policy.owner_only)
        throw std::invalid_argument("explicit caller roles required; owner policy requires entity dispatcher");
    auto& registered = impl_->handlers.at(route);
    registered.policy = policy; registered.legacy = false;
}
void Node::bind(std::string route, Handler handler) {
    if (route.empty() || route.size() > 256 || route.find('\0') != std::string::npos || !handler) throw std::invalid_argument("invalid RPC route");
    if (!impl_->handlers.emplace(std::move(route), Impl::Route{{true, true, false},
        [handler = std::move(handler)](const CallIdentity&, std::string data, Reply reply) { handler(std::move(data), std::move(reply)); }, true}).second) throw std::logic_error("duplicate RPC route");
}
void Node::call(std::uint64_t peer, std::string_view route, std::string_view data, std::chrono::milliseconds timeout, Reply reply) {
    auto keep = impl_;
    if (!reply) throw std::invalid_argument("missing reply callback");
    if (route.empty() || route.size() > 256 || route.find('\0') != std::string::npos || route.size() + data.size() > impl_->limits.frame_bytes - 14 || timeout.count() <= 0 || timeout > std::chrono::seconds(30))
        return keep->deliver(std::move(reply),{Status::rejected, {}});
    auto it = impl_->sessions.find(peer);
    if (impl_->stopped || it == impl_->sessions.end()) return keep->deliver(std::move(reply),{Status::disconnected, {}});
    if (impl_->calls.size() >= impl_->limits.pending_calls) return keep->deliver(std::move(reply),{Status::overloaded, {}});
    if (impl_->call_serial == std::numeric_limits<std::uint64_t>::max()) return keep->deliver(std::move(reply),{Status::overloaded, {}});
    auto id = ++impl_->call_serial;
    auto timer = std::make_shared<asio::steady_timer>(impl_->io, timeout);
    impl_->calls.emplace(id, Impl::Pending{peer, timer, std::move(reply)});
    timer->async_wait([weak = std::weak_ptr(impl_), id](asio::error_code error) {
        if (!error) if (auto node = weak.lock()) node->finish(id, {Status::timeout, {}});
    });
    if (!it->second->enqueue(frame(1, Status::ok, id, route, data))) impl_->finish(id, {Status::overloaded, {}});
}
bool Node::has_route(const std::string& route) const { return impl_->handlers.contains(route); }
bool Node::unbind(const std::string& route) {
    if (route == "system.health") return false;
    return impl_->handlers.erase(route) != 0;
}
void Node::on_notification(std::string channel, Notification handler) {
    if (channel.empty() || channel.size() > 256 || channel.find('\0') != std::string::npos || !handler)
        throw std::invalid_argument("invalid notification channel");
    if (!impl_->notifications.emplace(std::move(channel), std::move(handler)).second) throw std::logic_error("duplicate notification channel");
}
void Node::off_notification(const std::string& channel) { impl_->notifications.erase(channel); }
bool Node::notify(std::uint64_t peer, std::string_view channel, std::string_view data) {
    if (channel.empty() || channel.size() > 256 || channel.find('\0') != std::string::npos || channel.size() + data.size() > impl_->limits.frame_bytes - 14) return false;
    auto it = impl_->sessions.find(peer);
    if (impl_->stopped || it == impl_->sessions.end()) return false;
    return it->second->enqueue(frame(3, Status::ok, 0, channel, data));
}
bool Node::connected(std::uint64_t peer) const {
    auto it = impl_->sessions.find(peer);
    return !impl_->stopped && it != impl_->sessions.end() && it->second->authenticated && !it->second->closed;
}
bool Node::connecting(std::uint64_t peer) const {
    auto it=impl_->sessions.find(peer);
    return !impl_->stopped && it!=impl_->sessions.end() && !it->second->closed && !it->second->authenticated;
}
std::optional<CallIdentity> Node::identity(std::uint64_t peer) const {
    auto keep = impl_;
    auto it = keep->sessions.find(peer);
    if (keep->stopped || it == keep->sessions.end() || !it->second->authenticated || it->second->closed) return {};
    auto session = it->second;
    try {
        std::optional<CallIdentity> principal;
        if (keep->identity_resolver) { auto resolve = keep->identity_resolver; principal = resolve(peer, session->identity); }
        else principal=CallIdentity{Caller::client, session->identity, peer};
        if (!principal || principal->subject.size() > 256 || (principal->caller != Caller::client && principal->caller != Caller::server) || keep->stopped || session->closed) return {};
        principal->peer = peer; return principal;
    } catch (...) { return {}; }
}
void Node::close(std::uint64_t peer) {
    auto keep = impl_;
    auto it = keep->sessions.find(peer);
    if (it != keep->sessions.end()) { auto session = it->second; session->close(); }
}
void Node::poll() { auto keep = impl_; keep->drive(false); }
void Node::run() { auto keep = impl_; keep->drive(true); }
void Node::stop() {
    auto keep = impl_;
    if (!keep || std::exchange(keep->stopped, true)) return;
    asio::error_code ignored; keep->acceptor.close(ignored);
    auto sessions = keep->sessions;
    for (auto& [id, session] : sessions) session->close();
    keep->io.stop();
    // Drain canceled handlers to release shared ownership cycles before teardown.
    if (!keep->driving) keep->drain();
}
std::size_t Node::pending() const { return impl_->calls.size(); }
TransportStats Node::stats() const { TransportStats result;result.connections=impl_->sessions.size();result.pending=impl_->calls.size();result.receive_bytes=impl_->receive_bytes;result.callback_failures=impl_->callback_failures;for(const auto& [id,s]:impl_->sessions){(void)id;result.inflight+=s->inflight;result.queued_bytes+=s->queued;}return result; }
void Node::set_ready(bool value){impl_->ready=value;}
}

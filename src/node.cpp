#include "fsnet/node.hpp"
#include <asio.hpp>
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
std::string frame(unsigned kind, Status status, std::uint64_t id, const std::string& route, const std::string& data) {
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
    bool stopped = false;
    std::uint64_t peer_serial = 0, call_serial = 0;
    std::unordered_map<std::uint64_t, std::shared_ptr<Session>> sessions;
    std::unordered_map<std::uint64_t, Pending> calls;
    std::unordered_map<std::string, Handler> handlers;
    Impl(std::string auth, Limits settings) : token(std::move(auth)), limits(settings) {
        if (token.empty() || token.size() > 4096 || limits.frame_bytes < token.size() + 14 ||
            limits.frame_bytes > 16 * 1024 * 1024 || limits.queued_bytes < limits.frame_bytes + 4 ||
            !limits.connections || !limits.pending_calls || !limits.inflight_requests || limits.idle_timeout.count() <= 0)
            throw std::invalid_argument("invalid token or node limits");
    }
    void accept();
    void finish(std::uint64_t id, Result result) {
        auto it = calls.find(id);
        if (it == calls.end()) return;
        auto pending = std::move(it->second);
        calls.erase(it);
        pending.timer->cancel();
        pending.reply(std::move(result));
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
    asio::steady_timer idle;
    std::uint64_t id;
    bool closed = false, active = false, authenticated = false;
    std::size_t queued = 0, inflight = 0;
    std::array<char, 4> header{};
    std::string body;
    std::deque<std::shared_ptr<std::string>> output;
    Session(std::shared_ptr<Impl> node, std::uint64_t serial)
      : owner(node), socket(node->io), idle(node->io), id(serial) {
        enqueue(frame(0, Status::ok, 0, {}, node->token));
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
    void start() {
        asio::error_code error;
        socket.set_option(Tcp::no_delay(true), error);
        if (error) return close();
        active = true; deadline(); if (!output.empty()) write(); read_header();
    }
    void write() {
        if (closed || output.empty()) return;
        auto message = output.front();
        asio::async_write(socket, asio::buffer(*message),
          [self = shared_from_this(), message](asio::error_code error, std::size_t) {
            if (self->closed) return;
            if (error) return self->close();
            self->queued -= message->size(); self->output.pop_front(); self->write();
          });
    }
    void read_header() {
        asio::async_read(socket, asio::buffer(header),
          [self = shared_from_this()](asio::error_code error, std::size_t) {
            if (self->closed) return;
            auto node = self->owner.lock();
            auto size = get(self->header.data(), 4);
            if (error || !node || size < 14 || size > node->limits.frame_bytes) return self->close();
            self->body.resize(size);
            asio::async_read(self->socket, asio::buffer(self->body),
              [self](asio::error_code read_error, std::size_t) {
                if (self->closed) return;
                if (read_error) return self->close();
                self->deadline(); self->receive();
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
        if (kind != 1 || !call || route.empty() || code) return close();
        if (inflight >= node->limits.inflight_requests) {
            if (!enqueue(frame(2, Status::overloaded, call, {}, {}))) close();
            return;
        }
        auto it = node->handlers.find(route);
        if (it == node->handlers.end()) {
            if (!enqueue(frame(2, Status::not_found, call, {}, {}))) close();
            return;
        }
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
        auto handler = it->second;
        try { handler(std::move(data), reply); }
        catch (...) { reply({Status::internal, {}}); }
    }
};
void Node::Impl::accept() {
    if (stopped) return;
    auto session = std::make_shared<Session>(shared_from_this(), ++peer_serial);
    acceptor.async_accept(session->socket, [self = shared_from_this(), session](asio::error_code error) {
        if (self->stopped) return;
        if (!error && self->sessions.size() < self->limits.connections) {
            self->sessions.emplace(session->id, session); session->start();
        }
        self->accept();
    });
}
Node::Node(std::string token, Limits limits) : impl_(std::make_shared<Impl>(std::move(token), limits)) {
    bind("system.health", [](std::string, Reply reply) { reply({Status::ok, "ready"}); });
}
Node::~Node() { stop(); }
std::uint16_t Node::listen(const std::string& address, std::uint16_t port) {
    if (impl_->stopped || impl_->acceptor.is_open()) throw std::logic_error("node already listening or stopped");
    Tcp::endpoint endpoint(asio::ip::make_address(address), port);
    impl_->acceptor.open(endpoint.protocol());
    impl_->acceptor.set_option(Tcp::acceptor::reuse_address(true));
    impl_->acceptor.bind(endpoint); impl_->acceptor.listen(); impl_->accept();
    return impl_->acceptor.local_endpoint().port();
}
std::uint64_t Node::connect(const std::string& address, std::uint16_t port) {
    if (impl_->stopped || impl_->sessions.size() >= impl_->limits.connections) throw std::runtime_error("connection limit");
    Tcp::endpoint endpoint(asio::ip::make_address(address), port);
    auto session = std::make_shared<Impl::Session>(impl_, ++impl_->peer_serial);
    impl_->sessions.emplace(session->id, session);
    session->deadline();
    session->socket.async_connect(endpoint, [session](asio::error_code error) {
        if (session->closed) return;
        if (error) session->close(); else session->start();
    });
    return session->id;
}
void Node::bind(std::string route, Handler handler) {
    if (route.empty() || route.size() > 256 || !handler) throw std::invalid_argument("invalid RPC route");
    if (!impl_->handlers.emplace(std::move(route), std::move(handler)).second) throw std::logic_error("duplicate RPC route");
}
void Node::call(std::uint64_t peer, std::string route, std::string data, std::chrono::milliseconds timeout, Reply reply) {
    if (!reply) throw std::invalid_argument("missing reply callback");
    if (route.empty() || route.size() > 256 || route.size() + data.size() > impl_->limits.frame_bytes - 14 || timeout.count() <= 0 || timeout > std::chrono::seconds(30))
        return reply({Status::rejected, {}});
    auto it = impl_->sessions.find(peer);
    if (impl_->stopped || it == impl_->sessions.end()) return reply({Status::disconnected, {}});
    if (impl_->calls.size() >= impl_->limits.pending_calls) return reply({Status::overloaded, {}});
    if (impl_->call_serial == std::numeric_limits<std::uint64_t>::max()) return reply({Status::overloaded, {}});
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
void Node::close(std::uint64_t peer) {
    auto it = impl_->sessions.find(peer);
    if (it != impl_->sessions.end()) { auto session = it->second; session->close(); }
}
void Node::poll() { if (!impl_->stopped) { impl_->io.restart(); impl_->io.poll(); } }
void Node::run() { if (!impl_->stopped) { impl_->io.restart(); impl_->io.run(); } }
void Node::stop() {
    if (!impl_ || std::exchange(impl_->stopped, true)) return;
    asio::error_code ignored; impl_->acceptor.close(ignored);
    auto sessions = impl_->sessions;
    for (auto& [id, session] : sessions) session->close();
    impl_->io.stop();
    // Drain canceled handlers to release shared ownership cycles before teardown.
    impl_->io.restart(); impl_->io.poll(); impl_->io.stop();
}
std::size_t Node::pending() const { return impl_->calls.size(); }
}

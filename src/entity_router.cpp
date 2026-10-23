#include "fsnet/entity_router.hpp"
#include "fsnet/entity_directory.hpp"
#include "fsnet/entity_message.hpp"
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace fsnet {
namespace {
using Clock = std::chrono::steady_clock;
std::int64_t wall_time() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}
std::uint64_t score(const std::string& input) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (unsigned char byte : input) hash = (hash ^ byte) * 1099511628211ULL;
    return hash;
}
}
struct EntityRouter::Impl : std::enable_shared_from_this<Impl> {
    struct Member { EntityEndpoint endpoint; std::uint64_t peer = 0; Clock::time_point retry{}; };
    struct Call {
        std::uint64_t id;
        std::string type, entity, route, message, request_id;
        Reply reply;
        Clock::time_point deadline, next{};
        bool busy = false, done = false;
        std::size_t bytes;
    };
    Node& node;
    AsyncDatabase storage;
    EntityRouterLimits limits;
    std::vector<Member> members;
    std::unordered_map<std::uint64_t, std::shared_ptr<Call>> calls;
    std::size_t bytes = 0;
    std::uint64_t serial = 0;
    bool stopped = false, polling = false;
    Impl(Node& transport, std::shared_ptr<Database> database, std::vector<EntityEndpoint> endpoints, EntityRouterLimits settings)
      : node(transport), storage(std::move(database)), limits(settings) {
        if (endpoints.empty() || endpoints.size() > 1024 || !limits.requests || !limits.bytes) throw std::invalid_argument("invalid entity router");
        std::unordered_set<std::string> ids;
        for (auto& endpoint : endpoints) {
            if (endpoint.node.empty() || endpoint.node.size() > 127 || endpoint.node.find_first_of("/\0", 0, 2) != std::string::npos ||
                endpoint.address.empty() || !endpoint.port || endpoint.types.empty() || !ids.insert(endpoint.node).second)
                throw std::invalid_argument("invalid entity endpoint");
            members.push_back({std::move(endpoint)});
        }
    }
    void finish(const std::shared_ptr<Call>& call, Result result) {
        if (std::exchange(call->done, true)) return;
        calls.erase(call->id); bytes -= call->bytes;
        auto reply = std::move(call->reply); reply(std::move(result));
    }
    void failed(const std::shared_ptr<Call>& call, Status status) {
        if (call->done || stopped) return;
        if (call->request_id.empty() || (status != Status::rejected && status != Status::disconnected && status != Status::timeout && status != Status::overloaded))
            return finish(call, {status, {}});
        call->busy = false; call->next = Clock::now() + std::chrono::milliseconds(25);
    }
    void attempt(const std::shared_ptr<Call>& call) {
        auto location = std::make_shared<std::optional<EntityLocation>>();
        call->busy = true;
        if (!storage.submit(call->type.size() + call->entity.size(), [location, call](Database& db) {
            *location = entity_location(db, call->type, call->entity);
        }, [weak = weak_from_this(), location, call](std::exception_ptr error) {
            auto self = weak.lock();
            if (!self || self->stopped || call->done) return;
            if (Clock::now() >= call->deadline) return self->finish(call, {Status::timeout, {}});
            if (error) return self->finish(call, {Status::internal, {}});
            std::optional<std::size_t> selected;
            std::uint64_t best = 0;
            const bool owned = *location && (*location)->expires_ms > wall_time();
            for (std::size_t i = 0; i < self->members.size(); ++i) {
                const auto& member = self->members[i]; const auto& endpoint = member.endpoint;
                if (std::find(endpoint.types.begin(), endpoint.types.end(), call->type) == endpoint.types.end()) continue;
                if (owned) {
                    const auto& owner = (*location)->owner;
                    if (owner == endpoint.node || owner.starts_with(endpoint.node + '/')) { selected = i; break; }
                } else if (Clock::now() >= member.retry) {
                    auto value = score(call->type + '\0' + call->entity + '\0' + endpoint.node);
                    if (!selected || value > best) { selected = i; best = value; }
                }
            }
            if (!selected) return self->failed(call, Status::disconnected);
            auto& member = self->members[*selected];
            if (Clock::now() < member.retry) return self->failed(call, Status::disconnected);
            try {
                if (!member.peer) member.peer = self->node.connect(member.endpoint.address, member.endpoint.port);
                auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(call->deadline - Clock::now());
                auto attempt_timeout = std::clamp(remaining, std::chrono::milliseconds(1), std::chrono::milliseconds(1000));
                self->node.call(member.peer, call->route, call->message, attempt_timeout,
                    [weak, call, index = *selected](Result result) {
                        auto self = weak.lock();
                        if (!self || self->stopped || call->done) return;
                        if (result.status == Status::disconnected || result.status == Status::timeout) {
                            auto& member = self->members[index];
                            auto peer = std::exchange(member.peer, 0); member.retry = Clock::now() + std::chrono::milliseconds(250);
                            if (peer) self->node.close(peer);
                        }
                        if (result.status == Status::ok) self->finish(call, std::move(result));
                        else self->failed(call, result.status);
                    });
            } catch (...) { member.retry = Clock::now() + std::chrono::milliseconds(250); self->failed(call, Status::disconnected); }
        })) { call->busy = false; failed(call, Status::overloaded); }
    }
};
EntityRouter::EntityRouter(Node& node, std::shared_ptr<Database> database, std::vector<EntityEndpoint> endpoints, EntityRouterLimits limits)
  : impl_(std::make_shared<Impl>(node, std::move(database), std::move(endpoints), limits)) {}
EntityRouter::~EntityRouter() { stop(); }
void EntityRouter::call(std::string type, std::string id, std::string method, std::string payload,
    std::chrono::milliseconds timeout, Reply reply, std::string request_id) {
    auto& self = *impl_;
    if (!reply) throw std::invalid_argument("reply required");
    if (self.stopped) return reply({Status::disconnected, {}});
    if (type.empty() || type.size() > 127 || method.empty() || method.size() > 127 ||
        type.find_first_of(".\0", 0, 2) != std::string::npos || method.find_first_of(".\0", 0, 2) != std::string::npos ||
        id.empty() || id.size() > 256 || request_id.size() > 128 || timeout.count() <= 0 || timeout > std::chrono::minutes(5))
        return reply({Status::rejected, {}});
    auto bytes = type.size() + id.size() + method.size() + payload.size() + request_id.size() + 8;
    if (self.serial == UINT64_MAX || self.calls.size() >= self.limits.requests || bytes > self.limits.bytes - self.bytes)
        return reply({Status::overloaded, {}});
    auto call = std::make_shared<Impl::Call>(); call->id = ++self.serial;
    call->type = std::move(type); call->entity = std::move(id); call->route = call->type + '.' + method;
    call->message = entity_message(call->entity, payload, request_id); call->request_id = std::move(request_id);
    call->reply = std::move(reply); call->deadline = Clock::now() + timeout; call->bytes = bytes;
    self.calls.emplace(call->id, call); self.bytes += bytes;
}
std::size_t EntityRouter::poll(std::size_t budget) {
    auto keep_alive = impl_; auto& self = *keep_alive;
    if (self.stopped) return 0;
    if (self.polling) throw std::logic_error("recursive router poll");
    self.polling = true; struct Reset { bool& value; ~Reset() { value = false; } } reset{self.polling};
    auto work = self.storage.poll(budget);
    std::vector<std::shared_ptr<Impl::Call>> calls;
    for (const auto& [id, call] : self.calls) calls.push_back(call);
    for (auto& call : calls) {
        if (self.stopped || work >= budget) break;
        if (Clock::now() >= call->deadline) { self.finish(call, {Status::timeout, {}}); ++work; }
        else if (!call->busy && Clock::now() >= call->next) { self.attempt(call); ++work; }
    }
    return work;
}
std::size_t EntityRouter::pending() const { return impl_->calls.size(); }
void EntityRouter::stop() {
    auto keep_alive = impl_; auto& self = *keep_alive;
    if (std::exchange(self.stopped, true)) return;
    auto calls = self.calls;
    for (auto& [id, call] : calls) { try { self.finish(call, {Status::disconnected, {}}); } catch (...) {} }
    for (auto& member : self.members) if (auto peer = std::exchange(member.peer, 0)) self.node.close(peer);
    self.storage.stop();
}
void RoutedScriptHost::call_entity(std::string route, std::string data, std::chrono::milliseconds timeout, Reply reply) {
    auto split = route.find('.'); auto message = parse_entity_message(data);
    if (split == std::string::npos || !message) return reply({Status::rejected, {}});
    router_.call(route.substr(0, split), std::move(message->id), route.substr(split + 1), std::move(message->payload),
        timeout, std::move(reply), std::move(message->request_id));
}
}

#include "fsnet/entity_host.hpp"
#include "fsnet/entity_message.hpp"
#include "fsnet/entity_snapshot.hpp"
#include "fsnet/entity_record.hpp"
#include <algorithm>
#include <deque>
#include <limits>
#include <unordered_map>
#include <utility>

namespace fsnet {
namespace {
using Clock = std::chrono::steady_clock;
std::string key(const std::string& type, const std::string& id) { return type + '\0' + id; }
std::string record_key(const std::string& entity) { return "fsnet/entity/" + entity; }
std::int64_t wall_time() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

}
struct EntityScriptHost::Impl : std::enable_shared_from_this<Impl> {
    struct Method { std::string type, initial; Handler logic; };
    struct Request {
        std::shared_ptr<Method> method;
        std::string payload, route, request_id;
        Reply reply;
        Clock::time_point deadline;
        std::size_t bytes;
        bool tick = false;
    };
    struct Slot {
        EntityHandle handle;
        std::deque<Request> queue;
        bool busy = false, releasing = false, activated = false;
        Clock::time_point next_tick, touched;
    };
    struct Job {
        std::string entity, state, output, persisted;
        std::vector<EntityReceipt> receipts;
        bool replay = false;
        std::size_t resident = 0;
        std::uint64_t fence = 0, version = 0;
        std::int64_t expires = 0;
        Status status = Status::ok;
        Request request;
        bool responded = false, committing = false;
    };
    std::shared_ptr<ScriptHost> parent;
    AsyncDatabase storage;
    std::string owner;
    EntityRuntimeLimits limits;
    WallClock now;
    std::unordered_map<std::string, std::shared_ptr<Method>> methods;
    std::unordered_map<std::string, Slot> slots;
    std::deque<std::string> schedule;
    struct Release { Reply reply; bool done = false; };
    std::unordered_map<std::string, std::shared_ptr<Release>> releases;
    std::unordered_map<std::string, std::shared_ptr<Job>> active;
    EntityRuntimeStats counters;
    std::uint64_t generation = 0;
    bool stopped = false, polling = false;
    Impl(std::shared_ptr<ScriptHost> host, std::shared_ptr<Database> db, std::string id,
         EntityRuntimeLimits settings, StorageLimits storage_limits, WallClock time)
      : parent(std::move(host)), storage(std::move(db), storage_limits), owner(std::move(id)), limits(settings),
        now(time ? std::move(time) : WallClock(wall_time)) {
        if (!parent || owner.empty() || owner.size() > 256 || !limits.entities || !limits.requests ||
            !limits.queued_bytes || !limits.state_bytes || limits.state_bytes > 8 * 1024 * 1024 ||
            !limits.resident_bytes || !limits.receipts || limits.receipts > 1024 || !limits.receipt_bytes || limits.receipt_bytes > 8 * 1024 * 1024 || limits.state_bytes + limits.receipt_bytes > 16 * 1024 * 1024 - 300 ||
            limits.request_timeout.count() <= 0 || limits.lease <= limits.request_timeout ||
            limits.lease > std::chrono::hours(24) || limits.tick_interval.count() <= 0 || limits.idle_timeout.count() <= 0)
            throw std::invalid_argument("invalid entity runtime settings");
    }
    void complete(const std::shared_ptr<Job>& job, Result result) {
        if (std::exchange(job->responded, true)) return;
        active.erase(job->entity);
        auto it = slots.find(job->entity);
        if (it != slots.end()) { it->second.busy = false; it->second.touched = Clock::now();
            if (result.status == Status::ok) it->second.activated = true;
            else if (result.status == Status::rejected) it->second.activated = false; }
        --counters.pending; counters.queued_bytes -= job->request.bytes; counters.state_bytes -= job->resident; job->resident = 0;
        if (result.status == Status::ok) ++counters.committed;
        else if (result.status == Status::rejected) ++counters.rejected;
        else ++counters.failed;
        if (job->request.tick && result.status != Status::ok) ++counters.tick_failures;
        auto reply = std::move(job->request.reply);
        reply(std::move(result));
    }
    void enqueue(const std::string& route, std::string data, Reply reply, bool tick = false) {
        if (stopped) return reply({Status::disconnected, {}});
        auto method = methods.find(route);
        if (method == methods.end()) return reply({Status::not_found, {}});
        if (counters.pending >= limits.requests || data.size() > limits.queued_bytes - counters.queued_bytes)
            return reply({Status::overloaded, {}});
        auto message = parse_entity_message(data);
        if (!message) return reply({Status::rejected, {}});
        if (!message->request_id.empty() && message->payload.size() + route.size() + message->request_id.size() + 16 > limits.receipt_bytes)
            return reply({Status::overloaded, {}});
        auto entity = key(method->second->type, message->id);
        auto it = slots.find(entity);
        if (it == slots.end()) {
            if (slots.size() >= limits.entities) return reply({Status::overloaded, {}});
            auto time = Clock::now();
            it = slots.emplace(entity, Slot{{method->second->type, message->id, owner, ++generation}, {}, false, false, false,
                time + limits.tick_interval, time}).first;
            schedule.push_back(entity);
        }
        if (it->second.releasing) return reply({Status::rejected, {}});
        auto bytes = data.size();
        it->second.queue.push_back({method->second, std::move(message->payload), route, std::move(message->request_id), std::move(reply),
            Clock::now() + limits.request_timeout, bytes, tick});
        ++counters.pending; counters.queued_bytes += bytes;
    }
    void acquire(const std::string& entity, Slot& slot) {
        auto job = std::make_shared<Job>(); job->entity = entity;
        job->request = std::move(slot.queue.front()); slot.queue.pop_front(); slot.busy = true;
        active.emplace(entity, job);
        auto owner_id = owner; auto clock = now; auto settings = limits;
        auto accepted = storage.submit(job->request.bytes + limits.state_bytes + limits.receipt_bytes,
            [job, owner_id, clock, settings](Database& db) {
                for (int attempt = 0; attempt < 4; ++attempt) {
                    auto record = db.get(record_key(job->entity));
                    EntityRecord current;
                    if (record) current = decode_entity_record(record->value);
                    else {
                        // Never silently reset a record created by the earlier two-key protocol.
                        if (db.get("fsnet/actor/owner/" + job->entity) || db.get("fsnet/actor/state/" + job->entity))
                            throw std::runtime_error("legacy entity needs offline migration");
                        current.snapshot.state = job->request.method->initial;
                    }
                    auto time = clock();
                    if (time < 0 || time > std::numeric_limits<std::int64_t>::max() - settings.lease.count())
                        throw std::runtime_error("invalid lease clock");
                    if (current.owner != owner_id && current.expires_ms > time) { job->status = Status::rejected; return; }
                    job->state = current.snapshot.state; job->receipts = current.snapshot.receipts;
                    if (!job->request.request_id.empty()) for (const auto& receipt : job->receipts) {
                        if (receipt.id != job->request.request_id) continue;
                        if (receipt.route != job->request.route || receipt.request != job->request.payload) { job->status = Status::rejected; return; }
                        job->replay = true; job->output = receipt.response; break;
                    }
                    if (job->state.size() > settings.state_bytes) { job->status = Status::overloaded; return; }
                    // Reuse a sufficiently long owned lease. The final single-record
                    // CAS still detects takeover or administrative replacement.
                    if (record && current.owner == owner_id && current.expires_ms - time > settings.request_timeout.count()) {
                        job->version = record->version; job->fence = current.fence; job->expires = current.expires_ms; return;
                    }
                    if (current.fence >= static_cast<std::uint64_t>(INT64_MAX) - 1) throw std::overflow_error("entity fence exhausted");
                    current.owner = owner_id; current.expires_ms = time + settings.lease.count(); ++current.fence;
                    auto value = encode_entity_record(current);
                    if (!db.compare_exchange(record_key(job->entity), record ? record->version : 0, value)) continue;
                    auto granted = db.get(record_key(job->entity));
                    if (!granted || granted->value != value) { job->status = Status::rejected; return; }
                    job->version = granted->version; job->fence = current.fence; job->expires = current.expires_ms; return;
                }
                job->status = Status::rejected;
            }, [weak = weak_from_this(), job](std::exception_ptr error) {
                auto self = weak.lock();
                if (!self || self->stopped || job->responded) return;
                if (error) return self->complete(job, {Status::internal, {}});
                if (job->status != Status::ok) return self->complete(job, {job->status, {}});
                if (job->request.deadline <= Clock::now()) return self->complete(job, {Status::timeout, {}});
                if (job->replay) { ++self->counters.replayed; return self->complete(job, {Status::ok, std::move(job->output)}); }
                std::size_t retained = job->state.size();
                for (const auto& receipt : job->receipts) retained += receipt.bytes();
                if (retained > self->limits.resident_bytes - self->counters.state_bytes)
                    return self->complete(job, {Status::overloaded, {}});
                job->resident = retained; self->counters.state_bytes += retained;
                auto slot = self->slots.find(job->entity);
                if (slot == self->slots.end()) return self->complete(job, {Status::disconnected, {}});
                auto input = entity_message(slot->second.handle.id, state_message(job->state, job->request.payload));
                auto done = [weak, job](Result result) {
                    auto owner = weak.lock();
                    if (!owner || owner->stopped || job->responded || job->committing) return;
                    job->committing = true;
                    if (result.status != Status::ok) return owner->complete(job, std::move(result));
                    if (job->request.deadline <= Clock::now()) return owner->complete(job, {Status::timeout, {}});
                    auto next = parse_state_message(result.data);
                    if (!next) return owner->complete(job, {Status::internal, {}});
                    if (next->state.size() > owner->limits.state_bytes || next->payload.size() > owner->limits.queued_bytes)
                        return owner->complete(job, {Status::overloaded, {}});
                    job->state = std::move(next->state); job->output = std::move(next->payload);
                    if (!job->request.request_id.empty()) {
                        EntityReceipt receipt{job->request.request_id, job->request.route, job->request.payload, job->output};
                        if (receipt.bytes() > owner->limits.receipt_bytes) return owner->complete(job, {Status::overloaded, {}});
                        job->receipts.push_back(std::move(receipt));
                    }
                    std::size_t receipt_bytes = 0;
                    for (const auto& receipt : job->receipts) receipt_bytes += receipt.bytes();
                    while (!job->receipts.empty() && (job->receipts.size() > owner->limits.receipts || receipt_bytes > owner->limits.receipt_bytes)) {
                        receipt_bytes -= job->receipts.front().bytes(); job->receipts.erase(job->receipts.begin());
                    }
                    const auto retained = job->state.size() + job->output.size() + receipt_bytes + 12;
                    if (retained > owner->limits.resident_bytes - owner->counters.state_bytes + job->resident)
                        return owner->complete(job, {Status::overloaded, {}});
                    owner->counters.state_bytes = owner->counters.state_bytes - job->resident + retained;
                    job->resident = retained;
                    job->persisted = encode_entity_record({owner->owner, job->expires, job->fence,
                        {std::move(job->state), std::move(job->receipts)}});
                    auto clock = owner->now;
                    bool accepted = owner->storage.submit(job->persisted.size() + job->output.size(),
                        [job, clock](Database& db) {
                            if (clock() >= job->expires) { job->status = Status::rejected; return; }
                            if (!db.compare_exchange(record_key(job->entity), job->version, job->persisted)) job->status = Status::rejected;
                        }, [weak, job](std::exception_ptr error) {
                            auto owner = weak.lock();
                            if (!owner || owner->stopped || job->responded) return;
                            if (error) owner->complete(job, {Status::internal, {}});
                            else owner->complete(job, {job->status, job->status == Status::ok ? std::move(job->output) : std::string{}});
                        });
                    if (!accepted) owner->complete(job, {Status::overloaded, {}});
                };
                try { job->request.method->logic(std::move(input), done); }
                catch (...) { done({Status::internal, {}}); }
            });
        if (!accepted) complete(job, {Status::overloaded, {}});
    }
};
EntityScriptHost::EntityScriptHost(std::shared_ptr<ScriptHost> parent, std::shared_ptr<Database> db,
    std::string owner, EntityRuntimeLimits limits, StorageLimits storage, WallClock now)
  : impl_(std::make_shared<Impl>(std::move(parent), std::move(db), std::move(owner), limits, storage, std::move(now))) {}
EntityScriptHost::~EntityScriptHost() { stop(); }
bool EntityScriptHost::has_route(const std::string& route) const { return impl_->methods.contains(route) || impl_->parent->has_route(route); }
void EntityScriptHost::bind(std::string route, Handler handler) { impl_->parent->bind(std::move(route), std::move(handler)); }
void EntityScriptHost::bind_stateful(std::string route, std::string type, std::string initial, Handler logic) {
    auto& self = *impl_;
    if (self.stopped || type.empty() || type.size() > 127 || type.find_first_of(".\0", 0, 2) != std::string::npos ||
        !logic || initial.size() > self.limits.state_bytes || route.size() <= type.size() + 1 ||
        route.compare(0, type.size() + 1, type + '.') || self.parent->has_route(route))
        throw std::invalid_argument("invalid stateful route");
    for (const auto& [name, method] : self.methods)
        if (method->type == type && method->initial != initial) throw std::invalid_argument("inconsistent initial entity state");
    auto method = std::make_shared<Impl::Method>(Impl::Method{std::move(type), std::move(initial), std::move(logic)});
    self.methods.emplace(route, method);
    if (route == method->type + ".$tick") return; // runtime-only; clients cannot drive ticks
    try {
        self.parent->bind(route, [weak = std::weak_ptr(impl_), route](std::string data, Reply reply) {
            auto self = weak.lock();
            if (!self) return reply({Status::disconnected, {}});
            self->enqueue(route, std::move(data), std::move(reply));
        });
    } catch (...) { self.methods.erase(route); throw; }
}
void EntityScriptHost::unbind(const std::string& route) { impl_->parent->unbind(route); impl_->methods.erase(route); }
void EntityScriptHost::call_entity(std::string route, std::string data, std::chrono::milliseconds timeout, Reply reply) {
    impl_->parent->call_entity(std::move(route), std::move(data), timeout, std::move(reply));
}
std::uint64_t EntityScriptHost::connect(const std::string& address, std::uint16_t port) { return impl_->parent->connect(address, port); }
void EntityScriptHost::call(std::uint64_t peer, std::string route, std::string data, std::chrono::milliseconds timeout, Reply reply) {
    impl_->parent->call(peer, std::move(route), std::move(data), timeout, std::move(reply));
}
std::size_t EntityScriptHost::poll(std::size_t budget) {
    auto keep_alive = impl_; auto& self = *keep_alive;
    if (self.stopped) return 0;
    if (self.polling) throw std::logic_error("recursive entity poll");
    self.polling = true;
    struct Reset { bool& flag; ~Reset() { flag = false; } } reset{self.polling};
    auto work = self.storage.poll(budget);
    if (self.stopped) return work;
    auto time = Clock::now();
    auto remaining = self.schedule.size();
    while (remaining-- && !self.schedule.empty()) {
        if (work >= budget || self.stopped) break;
        auto entity = std::move(self.schedule.front()); self.schedule.pop_front();
        auto it = self.slots.find(entity);
        if (it == self.slots.end()) continue;
        auto& slot = it->second;
        if (!slot.busy && slot.queue.empty() && time - slot.touched >= self.limits.idle_timeout) { self.slots.erase(it); continue; }
        self.schedule.push_back(entity);
        if (slot.busy) continue;
        if (slot.queue.empty() && time >= slot.next_tick) {
            auto route = slot.handle.type + ".$tick";
            if (slot.activated && self.methods.contains(route)) self.enqueue(route, entity_message(slot.handle.id,
                std::to_string(self.limits.tick_interval.count())), [](Result) {}, true);
            slot.next_tick = time + self.limits.tick_interval;
        }
        if (slot.queue.empty()) continue;
        if (slot.queue.front().deadline <= time) {
            auto request = std::move(slot.queue.front()); slot.queue.pop_front();
            --self.counters.pending; self.counters.queued_bytes -= request.bytes; ++self.counters.failed;
            request.reply({Status::timeout, {}});
        } else self.acquire(entity, slot);
        ++work;
    }
    return work;
}
std::optional<EntityHandle> EntityScriptHost::handle(const std::string& type, const std::string& id) const {
    auto it = impl_->slots.find(key(type, id));
    return it == impl_->slots.end() ? std::nullopt : std::optional(it->second.handle);
}
void EntityScriptHost::deactivate(const EntityHandle& handle, Reply reply) {
    auto& self = *impl_; auto entity = key(handle.type, handle.id); auto it = self.slots.find(entity);
    if (self.stopped) return reply({Status::disconnected, {}});
    if (handle.owner != self.owner || it == self.slots.end() || it->second.handle.generation != handle.generation || it->second.busy || !it->second.queue.empty())
        return reply({Status::rejected, {}});
    it->second.busy = true; it->second.releasing = true;
    auto release = std::make_shared<Impl::Release>(Impl::Release{std::move(reply)});
    self.releases.emplace(entity, release);
    auto status = std::make_shared<Status>(Status::rejected); auto id = self.owner;
    auto done = [weak = std::weak_ptr(impl_), entity, status, release](std::exception_ptr error) {
        if (std::exchange(release->done, true)) return;
        auto owner = weak.lock();
        if (!owner || owner->stopped) return release->reply({Status::disconnected, {}});
        owner->releases.erase(entity);
        if (!error && *status == Status::ok) { owner->slots.erase(entity); std::erase(owner->schedule, entity); }
        else if (auto it = owner->slots.find(entity); it != owner->slots.end()) { it->second.busy = false; it->second.releasing = false; }
        release->reply({error ? Status::internal : *status, {}});
    };
    if (!self.storage.submit(entity.size() + id.size(), [entity, id, status](Database& db) {
        auto record = db.get(record_key(entity));
        if (!record) return;
        auto value = decode_entity_record(record->value);
        if (value.owner != id) return;
        value.owner.clear(); value.expires_ms = 0;
        if (db.compare_exchange(record_key(entity), record->version, encode_entity_record(value))) *status = Status::ok;
    }, done)) { it->second.busy = false; done(std::make_exception_ptr(std::runtime_error("storage overloaded"))); }
}
EntityRuntimeStats EntityScriptHost::stats() const { auto result = impl_->counters; result.entities = impl_->slots.size(); return result; }
void EntityScriptHost::stop() {
    auto keep_alive = impl_; auto& self = *keep_alive;
    if (std::exchange(self.stopped, true)) return;
    for (const auto& [route, method] : self.methods) self.parent->unbind(route);
    self.methods.clear();
    auto active = self.active;
    for (auto& [entity, job] : active) {
        try { self.complete(job, {Status::disconnected, {}}); } catch (...) {}
    }
    auto slots = std::move(self.slots); self.slots.clear(); self.schedule.clear();
    for (auto& [entity, slot] : slots) for (auto& request : slot.queue) {
        --self.counters.pending; self.counters.queued_bytes -= request.bytes;
        try { request.reply({Status::disconnected, {}}); } catch (...) {}
    }
    auto releases = std::move(self.releases); self.releases.clear();
    for (auto& [entity, release] : releases) if (!std::exchange(release->done, true)) {
        try { release->reply({Status::disconnected, {}}); } catch (...) {}
    }
    self.storage.stop();
}
}

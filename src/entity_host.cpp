#include "fsnet/entity_host.hpp"
#include "fsnet/entity_message.hpp"
#include "fsnet/entity_snapshot.hpp"
#include "fsnet/entity_record.hpp"
#include <algorithm>
#include <openssl/sha.h>
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
    struct Method { std::string type, initial; ContextHandler logic; std::optional<RpcPolicy> policy; std::shared_ptr<const EntitySchema> schema; };
    struct Request {
        std::shared_ptr<Method> method;
        std::string payload, route, request_id;
        Reply reply;
        Clock::time_point deadline;
        std::size_t bytes;
        bool tick = false;
        CallIdentity identity{Caller::server, {}};
    };
    struct Slot {
        EntityHandle handle;
        std::deque<Request> queue;
        bool busy = false, releasing = false, activated = false;
        Clock::time_point next_tick, touched;
        std::string transient;
        std::uint64_t transient_fence = 0, transient_revision = 0;
    };
    struct Job {
        std::string entity, state, output, persisted;
        std::vector<EntityReceipt> receipts;
        bool replay = false, created = false;
        std::size_t resident = 0;
        std::uint64_t fence = 0, version = 0;
        std::int64_t expires = 0;
        Status status = Status::ok;
        Request request;
        bool responded = false, committing = false, previously_owned = false;
        std::uint64_t previous_fence = 0;
        std::optional<AttributeCommit> attributes;
        std::string transient;
        std::size_t cache_growth = 0;
    };
    std::shared_ptr<ScriptHost> parent;
    AsyncDatabase storage;
    std::string owner;
    EntityRuntimeLimits limits;
    WallClock now;
    OwnerResolver owner_resolver;
    AttributeListener attribute_listener;
    Visibility visibility;
    std::unordered_map<std::string, std::shared_ptr<const EntitySchema>> schemas;
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
    std::string account(const std::string& type, const std::string& id) const {
        auto definition=schemas.find(type);
        if (definition!=schemas.end() && definition->second->owner_from_id) return id;
        auto resolve=owner_resolver; return resolve ? resolve(type,id) : std::string{};
    }
    void complete(const std::shared_ptr<Job>& job, Result result) {
        if (std::exchange(job->responded, true)) return;
        active.erase(job->entity);
        auto it = slots.find(job->entity);
        if (job->attributes && result.status == Status::ok && it != slots.end()) {
            auto& slot = it->second;
            if (slot.transient.size() > job->transient.size()) counters.transient_bytes -= slot.transient.size() - job->transient.size();
            slot.transient = std::move(job->transient); slot.transient_fence = job->fence;
            slot.transient_revision = job->attributes->current.revision;
        } else counters.transient_bytes -= job->cache_growth;
        job->cache_growth = 0;
        if (it != slots.end()) { it->second.busy = false; it->second.touched = Clock::now();
            if (result.status == Status::ok) it->second.activated = true;
            else if (result.status == Status::rejected) it->second.activated = false; }
        --counters.pending; counters.queued_bytes -= job->request.bytes; counters.state_bytes -= job->resident; job->resident = 0;
        if (result.status == Status::ok) ++counters.committed;
        else if (result.status == Status::rejected) ++counters.rejected;
        else ++counters.failed;
        if (job->request.tick && result.status != Status::ok) ++counters.tick_failures;
        auto reply = std::move(job->request.reply);
        if (result.status == Status::ok && job->attributes && attribute_listener &&
            (job->attributes->previous.values != job->attributes->current.values || job->attributes->previous.revision != job->attributes->current.revision)) {
            try { auto listener = attribute_listener; listener(*job->attributes); } catch (...) { ++counters.publication_failures; }
        }
        reply(std::move(result));
    }
    void enqueue(const std::string& route, std::string data, Reply reply, bool tick = false, CallIdentity identity = {Caller::server, {}}) {
        if (stopped) return reply({Status::disconnected, {}});
        auto method = methods.find(route);
        if (method == methods.end()) return reply({Status::not_found, {}});
        auto selected = method->second;
        if (counters.pending >= limits.requests || data.size() > limits.queued_bytes - counters.queued_bytes)
            return reply({Status::overloaded, {}});
        auto message = parse_entity_message(data);
        if (!message) return reply({Status::rejected, {}});
        if (!message->request_id.empty() && message->payload.size() + route.size() + message->request_id.size() + 16 > limits.receipt_bytes)
            return reply({Status::overloaded, {}});
        if (selected->policy) {
            try {
                auto policy = *selected->policy;
                std::string account;
                if (policy.owner_only) account = this->account(selected->type, message->id);
                if (!policy.allows(identity, account)) return reply({Status::rejected, {}});
            } catch (...) { return reply({Status::rejected, {}}); }
            if (stopped) return reply({Status::disconnected, {}});
            if (!methods.contains(route) || methods.at(route) != selected) return reply({Status::rejected, {}});
            // Receipts must not disclose another principal's response, even if
            // two callers choose the same request ID and payload.
            if (!message->request_id.empty()) {
                auto subject = identity.subject;
                if (subject.empty()) subject = "anonymous:" + owner + ':' + std::to_string(identity.peer);
                auto scope = std::string(identity.caller == Caller::server ? "server:" : "client:") +
                    std::to_string(subject.size()) + ':' + subject + message->request_id;
                if (identity.subject.empty()) scope.insert(0, "anonymous:");
                unsigned char digest[SHA256_DIGEST_LENGTH];
                SHA256(reinterpret_cast<const unsigned char*>(scope.data()), scope.size(), digest);
                constexpr char hex[] = "0123456789abcdef";
                message->request_id = "context:";
                for (auto byte : digest) { message->request_id += hex[byte >> 4]; message->request_id += hex[byte & 15]; }
            }
        }
        auto entity = key(selected->type, message->id);
        auto it = slots.find(entity);
        if (it == slots.end()) {
            if (slots.size() >= limits.entities) return reply({Status::overloaded, {}});
            auto time = Clock::now();
            it = slots.emplace(entity, Slot{{selected->type, message->id, owner, ++generation}, {}, false, false, false,
                time + limits.tick_interval, time, {}, 0, 0}).first;
            schedule.push_back(entity);
        }
        if (it->second.releasing) return reply({Status::rejected, {}});
        auto bytes = data.size();
        it->second.queue.push_back({selected, std::move(message->payload), route, std::move(message->request_id), std::move(reply),
            Clock::now() + limits.request_timeout, bytes, tick, std::move(identity)});
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
                    job->created = !record;
                    EntityRecord current;
                    if (record) current = decode_entity_record(record->value);
                    else {
                        // Never silently reset a record created by the earlier two-key protocol.
                        if (db.get("fsnet/actor/owner/" + job->entity) || db.get("fsnet/actor/state/" + job->entity))
                            throw std::runtime_error("legacy entity needs offline migration");
                        current.snapshot.state = job->request.method->initial;
                        if(job->request.method->schema && job->request.route==job->request.method->type+".$spawn") {
                            EntityAttributes initial(*job->request.method->schema,"");
                            initial.publish(initial.stage(decode_attributes(job->request.payload).values,{Caller::server,{}}));
                            current.snapshot.state=encode_attributes({0,initial.revision(),true,initial.persistent()});
                        }
                    }
                    auto time = clock();
                    if (time < 0 || time > std::numeric_limits<std::int64_t>::max() - settings.lease.count())
                        throw std::runtime_error("invalid lease clock");
                    if (current.owner != owner_id && current.expires_ms > time) { job->status = Status::rejected; return; }
                    job->state = current.snapshot.state; job->receipts = current.snapshot.receipts;
                    job->previously_owned = current.owner == owner_id; job->previous_fence = current.fence;
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
                // A queued or suspended call may have lost account ownership.
                // Recheck before business execution; ownership mapping changes must
                // themselves be serialized with the entity lifecycle.
                if (job->request.method->policy && job->request.method->policy->owner_only) {
                    try {
                        auto current_slot = self->slots.find(job->entity);
                        if (current_slot == self->slots.end()) return self->complete(job, {Status::disconnected, {}});
                        auto account = self->account(current_slot->second.handle.type, current_slot->second.handle.id);
                        if (!job->request.method->policy->allows(job->request.identity, account)) return self->complete(job, {Status::rejected, {}});
                    } catch (...) { return self->complete(job, {Status::rejected, {}}); }
                }
                if (job->replay) { ++self->counters.replayed; return self->complete(job, {Status::ok, std::move(job->output)}); }
                std::size_t retained = job->state.size();
                for (const auto& receipt : job->receipts) retained += receipt.bytes();
                if (retained > self->limits.resident_bytes - self->counters.state_bytes)
                    return self->complete(job, {Status::overloaded, {}});
                job->resident = retained; self->counters.state_bytes += retained;
                auto slot = self->slots.find(job->entity);
                if (slot == self->slots.end()) return self->complete(job, {Status::disconnected, {}});
                if (job->request.method->schema) {
                    try {
                        auto image = decode_attributes(job->state);
                        if (!image.snapshot) throw std::runtime_error("persistent properties require a snapshot");
                        EntityAttributes model(*job->request.method->schema, ""); model.restore(image.values, image.revision);
                        auto& cached = slot->second;
                        if (!cached.transient.empty() && job->previously_owned && cached.transient_fence == job->previous_fence && cached.transient_revision == image.revision) {
                            auto transient = decode_attributes(cached.transient);
                            model.restore_full(transient.values, image.revision);
                        }
                        if(job->created && job->request.route==job->request.method->type+".$spawn") {
                            auto requested=decode_attributes(job->request.payload).values;
                            EntityAttributes::Values transient;
                            for(const auto& [name,value]:requested)
                                if(!model.schema().attributes.at(name).persist) transient.emplace(name,value);
                            model.restore_full(transient,image.revision);
                        }
                        AttributeUpdate full{0, image.revision, true, model.all_values()};
                        auto bytes = encode_attributes(full);
                        auto retained = job->resident + 4 * bytes.size() + 65536;
                        if (retained > self->limits.resident_bytes - self->counters.state_bytes + job->resident)
                            return self->complete(job, {Status::overloaded, {}});
                        self->counters.state_bytes = self->counters.state_bytes - job->resident + retained;
                        job->resident = retained; job->state = std::move(bytes);
                        auto entity_handle = slot->second.handle;
                        auto account = self->account(entity_handle.type, entity_handle.id);
                        if (self->stopped || job->responded) return;
                        job->attributes = AttributeCommit{entity_handle.type, entity_handle.id, account, job->request.method->schema, full, {}};
                    } catch (...) { return self->complete(job, {Status::internal, {}}); }
                }
                if (job->attributes && job->request.route == job->attributes->type + ".$write") {
                    try {
                        auto proposal = decode_attributes(job->request.payload);
                        if (proposal.snapshot || proposal.base != job->attributes->previous.revision || proposal.revision != proposal.base)
                            throw std::invalid_argument("stale property write proposal");
                        EntityAttributes model(*job->attributes->schema, job->attributes->owner);
                        model.restore_full(job->attributes->previous.values, job->attributes->previous.revision);
                        (void)model.stage(proposal.values, job->request.identity);
                    } catch (...) { return self->complete(job, {Status::rejected, {}}); }
                }
                auto current_slot = self->slots.find(job->entity);
                if (self->stopped || job->responded || current_slot == self->slots.end()) return;
                auto input = entity_message(current_slot->second.handle.id, state_message(job->state, job->request.payload));
                auto done = [weak, job](Result result) {
                    auto owner = weak.lock();
                    if (!owner || owner->stopped || job->responded || job->committing) return;
                    job->committing = true;
                    if (result.status != Status::ok) return owner->complete(job, std::move(result));
                    if (job->request.deadline <= Clock::now()) return owner->complete(job, {Status::timeout, {}});
                    if (job->request.method->policy && job->request.method->policy->owner_only) {
                        try {
                            auto slot = owner->slots.find(job->entity);
                            auto account = slot != owner->slots.end() ? owner->account(slot->second.handle.type, slot->second.handle.id) : std::string{};
                            if (!job->request.method->policy->allows(job->request.identity, account)) return owner->complete(job, {Status::rejected, {}});
                        } catch (...) { return owner->complete(job, {Status::rejected, {}}); }
                    }
                    auto next = parse_state_message(result.data);
                    if (!next) return owner->complete(job, {Status::internal, {}});
                    if (next->state.size() > owner->limits.state_bytes || next->payload.size() > owner->limits.queued_bytes)
                        return owner->complete(job, {Status::overloaded, {}});
                    if (job->attributes) {
                        try {
                            auto image = decode_attributes(next->state);
                            if (!image.snapshot || image.revision < job->attributes->previous.revision ||
                                (image.revision == job->attributes->previous.revision && image.values != job->attributes->previous.values))
                                throw std::runtime_error("invalid property revision");
                            EntityAttributes model(*job->attributes->schema, ""); model.restore_full(image.values, image.revision);
                            job->attributes->current = {0, image.revision, true, model.all_values()};
                            EntityAttributes::Values transient;
                            for (const auto& [name, definition] : job->attributes->schema->attributes)
                                if (!definition.persist) transient.emplace(name, model.all_values().at(name));
                            if (!transient.empty()) job->transient = encode_attributes({0, image.revision, true, std::move(transient)});
                            next->state = encode_attributes({0, image.revision, true, model.persistent()});
                        } catch (...) { return owner->complete(job, {Status::internal, {}}); }
                    }
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
                    const auto retained = (job->attributes ? 5 : 1) * job->state.size() + job->transient.size() * 4 + job->output.size() + receipt_bytes + 12;
                    if (retained > owner->limits.resident_bytes - owner->counters.state_bytes + job->resident)
                        return owner->complete(job, {Status::overloaded, {}});
                    owner->counters.state_bytes = owner->counters.state_bytes - job->resident + retained;
                    job->resident = retained;
                    if (job->attributes) {
                        auto found = owner->slots.find(job->entity);
                        if (found == owner->slots.end()) return owner->complete(job, {Status::disconnected, {}});
                        auto previous = found->second.transient.size();
                        auto growth = job->transient.size() > previous ? job->transient.size() - previous : 0;
                        if (growth > owner->limits.resident_bytes - owner->counters.transient_bytes)
                            return owner->complete(job, {Status::overloaded, {}});
                        owner->counters.transient_bytes += growth; job->cache_growth = growth;
                    }
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
                try { job->request.method->logic(job->request.identity, std::move(input), done); }
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
void EntityScriptHost::rpc_policy(const std::string& route, RpcPolicy policy) { impl_->parent->rpc_policy(route, policy); }
void EntityScriptHost::bind(std::string route, Handler handler) { impl_->parent->bind(std::move(route), std::move(handler)); }
void EntityScriptHost::bind_stateful(std::string route, std::string type, std::string initial, Handler logic) {
    auto& self = *impl_;
    if (self.stopped || type.empty() || type.size() > 127 || type.find_first_of(".\0", 0, 2) != std::string::npos ||
        !logic || self.methods.contains(route) || initial.size() > self.limits.state_bytes || route.size() <= type.size() + 1 ||
        route.compare(0, type.size() + 1, type + '.') || self.parent->has_route(route))
        throw std::invalid_argument("invalid stateful route");
    for (const auto& [name, method] : self.methods)
        if (method->type == type && method->initial != initial) throw std::invalid_argument("inconsistent initial entity state");
    auto schema = self.schemas.contains(type) ? self.schemas.at(type) : nullptr;
    auto method = std::make_shared<Impl::Method>(Impl::Method{std::move(type), std::move(initial),
        [logic = std::move(logic)](const CallIdentity&, std::string data, Reply reply) { logic(std::move(data), std::move(reply)); }, {}, std::move(schema)});
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
void EntityScriptHost::define_schema(std::string type, EntitySchema schema) {
    schema.validate();
    if (impl_->stopped || type.empty() || type.size() > 127 || type.find_first_of(".\0", 0, 2) != std::string::npos || impl_->schemas.size() >= 256)
        throw std::invalid_argument("invalid entity schema type");
    for (const auto& [route, method] : impl_->methods) if (method->type == type) throw std::logic_error("entity schema must precede methods");
    auto definition = std::make_shared<const EntitySchema>(std::move(schema));
    EntityAttributes defaults(*definition, "");
    auto initial = encode_attributes({0, 0, true, defaults.persistent()});
    if (!impl_->schemas.emplace(type, definition).second) throw std::logic_error("duplicate entity schema");
    try {
        auto method = std::make_shared<Impl::Method>(Impl::Method{type, std::move(initial),
            [](const CallIdentity&, std::string input, Reply reply) {
                auto entity = parse_entity_message(input);
                auto state = entity ? parse_state_message(entity->payload) : std::nullopt;
                if (!state) return reply({Status::internal, {}});
                reply({Status::ok, state_message(state->state, state->state)});
            }, RpcPolicy{false, true, false}, std::move(definition)});
        impl_->methods.emplace(type + ".$snapshot", method);
        impl_->methods.emplace(type + ".$spawn", std::move(method));
    } catch (...) { impl_->schemas.erase(type); impl_->methods.erase(type+".$snapshot"); impl_->methods.erase(type+".$spawn"); throw; }
}
void EntityScriptHost::forget_schema(const std::string& type) { impl_->schemas.erase(type); impl_->methods.erase(type + ".$snapshot"); impl_->methods.erase(type + ".$spawn"); }
std::shared_ptr<const EntitySchema> EntityScriptHost::schema(const std::string& type) const {
    auto it = impl_->schemas.find(type); return it == impl_->schemas.end() ? nullptr : it->second;
}
std::string EntityScriptHost::owner_subject(const std::string& type, const std::string& id) const {
    return impl_->account(type,id);
}
void EntityScriptHost::spawn_entity(std::string type,std::string id,EntityAttributes::Values values,Reply reply) {
    std::string message;
    try {
        auto definition=schema(type); if(!definition) return reply({Status::not_found,{}});
        EntityAttributes model(*definition,"");
        (void)model.stage(values,{Caller::server,{}});
        message=entity_message(id,encode_attributes({0,0,true,std::move(values)}));
    } catch(...) { return reply({Status::rejected,{}}); }
    impl_->enqueue(type+".$spawn",std::move(message),std::move(reply));
}
void EntityScriptHost::on_visibility(Visibility fn) { impl_->visibility=std::move(fn); }
void EntityScriptHost::entity_visibility(std::uint64_t peer,std::string type,std::string id,unsigned action,Reply reply) {
    auto fn=impl_->visibility;
    if(impl_->stopped || !fn) return reply({Status::rejected,{}});
    fn(peer,std::move(type),std::move(id),action,std::move(reply));
}
void EntityScriptHost::read_attributes(std::string type, std::string id, Reply reply) {
    if (!reply) throw std::invalid_argument("missing snapshot callback");
    std::string message;
    try { message = entity_message(id, ""); } catch (...) { return reply({Status::rejected, {}}); }
    impl_->enqueue(type + ".$snapshot", std::move(message), std::move(reply), false, {Caller::server, "framework:snapshot"});
}
void EntityScriptHost::write_attributes(std::string type, std::string id, CallIdentity who, std::string proposal, Reply reply, std::string request_id) {
    if (!reply) throw std::invalid_argument("missing write callback");
    std::string message;
    try { message = entity_message(id, proposal, request_id); } catch (...) { return reply({Status::rejected, {}}); }
    impl_->enqueue(type + ".$write", std::move(message), std::move(reply), false, std::move(who));
}
void EntityScriptHost::on_attributes(AttributeListener listener) { impl_->attribute_listener = std::move(listener); }
void EntityScriptHost::ownership(OwnerResolver resolve) { impl_->owner_resolver = std::move(resolve); }
void EntityScriptHost::bind_context(std::string route, RpcPolicy policy, ContextHandler handler) {
    impl_->parent->bind_context(std::move(route), policy, std::move(handler));
}
void EntityScriptHost::bind_stateful_context(std::string route, std::string type, std::string initial, RpcPolicy policy, ContextHandler handler) {
    if (!handler || (!policy.client && !policy.server)) throw std::invalid_argument("explicit contextual handler required");
    bind_stateful(route, std::move(type), std::move(initial), [](std::string, Reply reply) { reply({Status::internal, {}}); });
    auto method = impl_->methods.at(route);
    method->logic = std::move(handler); method->policy = policy;
    if (route == method->type + ".$tick") return;
    impl_->parent->unbind(route);
    auto transport_policy = policy; transport_policy.owner_only = false;
    try {
        impl_->parent->bind_context(route, transport_policy,
            [weak = std::weak_ptr(impl_), route](const CallIdentity& identity, std::string data, Reply reply) {
                auto self = weak.lock();
                if (!self) return reply({Status::disconnected, {}});
                self->enqueue(route, std::move(data), std::move(reply), false, identity);
            });
    } catch (...) { impl_->methods.erase(route); throw; }
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
        if (!slot.busy && slot.queue.empty() && time - slot.touched >= self.limits.idle_timeout) { self.counters.transient_bytes -= slot.transient.size(); self.slots.erase(it); continue; }
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
bool EntityScriptHost::active(const std::string& type,const std::string& id) const {
    auto it=impl_->slots.find(key(type,id));
    return !impl_->stopped && it!=impl_->slots.end() && it->second.activated && !it->second.releasing;
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
        if (!error && *status == Status::ok) { auto it = owner->slots.find(entity); if (it != owner->slots.end()) owner->counters.transient_bytes -= it->second.transient.size(); owner->slots.erase(entity); std::erase(owner->schedule, entity); }
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
    auto slots = std::move(self.slots); self.slots.clear(); self.schedule.clear(); self.counters.transient_bytes = 0;
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

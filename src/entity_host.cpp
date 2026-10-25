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
std::string record_key(const std::string& entity) {
    auto split=entity.find('\0');return entity_record_key(entity.substr(0,split),entity.substr(split+1));
}
std::int64_t wall_time() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

}
struct EntityScriptHost::Impl : std::enable_shared_from_this<Impl> {
    struct Method { std::string type, initial; ContextHandler logic; std::optional<RpcPolicy> policy; std::shared_ptr<const EntitySchema> schema; };
    struct DeliveryAck { std::uint64_t epoch; std::string ticket; Result result; };
    struct Request {
        std::shared_ptr<Method> method;
        std::string payload, route, request_id;
        Reply reply;
        Clock::time_point deadline;
        std::size_t bytes;
        bool tick = false;
        CallIdentity identity{Caller::server, {}};
        std::string handoff_target;
        std::uint64_t accept_transfer=0;
        std::optional<DeliveryEnvelope> incoming;
        std::optional<DeliveryAck> ack;
        std::optional<Result> delivery_error;
    };
    struct Slot {
        EntityHandle handle;
        std::deque<Request> queue;
        bool busy = false, releasing = false, activated = false;
        std::uint64_t activation_fence = 0;
        Clock::time_point next_tick, touched;
        std::string transient;
        std::uint64_t transient_fence = 0, transient_revision = 0;
        std::string account{};
    };
    struct Job {
        std::string entity, state, output, persisted;
        std::vector<EntityReceipt> receipts;
        bool replay = false, created = false, activating = false, owner_lost = false, wrong_node = false, preserve_activation = false;
        std::size_t resident = 0;
        std::uint64_t fence = 0, version = 0;
        std::int64_t expires = 0;
        Status status = Status::ok;
        Request request;
        bool responded = false, committing = false, previously_owned = false;
        std::uint64_t previous_fence = 0;
        std::optional<AttributeCommit> attributes;
        std::string transient, placement;
        std::uint64_t transfer_id=0;
        std::size_t cache_growth = 0;
        EntityDeliveryState delivery;
        std::optional<EntityDeliveryState> delivery_rollback;
        std::string rollback;
        bool metadata_only=false, logic_started=false, commit_uncertain=false;
        std::optional<Result> delivery_result;
        std::optional<EntityMetadata> metadata;
    };
    struct Admission {
        std::uint64_t id;std::size_t bytes;Reply reply;Clock::time_point deadline;bool done=false;
    };
    std::shared_ptr<ScriptHost> parent;
    std::shared_ptr<EntityCatalog> catalog;
    AsyncDatabase storage;
    std::string owner;
    EntityRuntimeLimits limits;
    WallClock now;
    OwnerResolver owner_resolver;
    AttributeListener attribute_listener;
    Closed closed_listener;
    Visibility visibility;
    Transfer transfer;
    CreateOnNode create_on_node;
    std::unordered_map<std::string, std::shared_ptr<const EntitySchema>> schemas;
    std::unordered_map<std::string, std::shared_ptr<Method>> methods;
    std::unordered_map<std::string, Slot> slots;
    std::deque<std::string> schedule;
    struct Release { Reply reply; bool done = false; };
    std::unordered_map<std::string, std::shared_ptr<Release>> releases;
    std::unordered_map<std::string, std::shared_ptr<Job>> active;
    std::unordered_map<std::uint64_t,std::shared_ptr<Admission>> admissions;
    std::unordered_map<std::string,std::string> resolved_types;
    std::uint64_t admission_serial=0;
    bool global_entry=false;
    EntityRuntimeStats counters;
    std::uint64_t generation = 0;
    bool stopped = false, polling = false, draining = false, delivery_enabled = false;
    Impl(std::shared_ptr<ScriptHost> host, std::shared_ptr<Database> db, std::string id,
         EntityRuntimeLimits settings, StorageLimits storage_limits, WallClock time)
      : parent(std::move(host)), catalog(db?std::make_shared<EntityCatalog>(*db):nullptr), storage(std::move(db), storage_limits), owner(std::move(id)), limits(settings),
        now(time ? std::move(time) : WallClock(wall_time)) {
        if (!parent || owner.empty() || owner.size() > 256 || !limits.entities || !limits.requests ||
            !limits.queued_bytes || !limits.state_bytes || limits.state_bytes > 8 * 1024 * 1024 ||
            !limits.resident_bytes || !limits.receipts || limits.receipts > 1024 || !limits.receipt_bytes || limits.receipt_bytes > 8 * 1024 * 1024 || limits.state_bytes + limits.receipt_bytes + limits.delivery_bytes > 16 * 1024 * 1024 - 512 ||
            !limits.delivery_messages || limits.delivery_messages>1024 || !limits.delivery_channels || limits.delivery_channels>4096 ||
            !limits.delivery_sources || limits.delivery_sources>65536 || !limits.delivery_bytes || limits.delivery_bytes>8*1024*1024 ||
            limits.request_timeout.count() <= 0 || limits.lease <= limits.request_timeout ||
            limits.lease > std::chrono::hours(24) || limits.tick_interval.count() <= 0 || limits.idle_timeout.count() <= 0)
            throw std::invalid_argument("invalid entity runtime settings");
    }
    std::string account(const std::string& type, const std::string& id) const {
        if(valid_entity_id(id)) {
            auto found=slots.find(key(type,id));return found==slots.end()?std::string{}:found->second.account;
        }
        auto definition=schemas.find(type);
        if (definition!=schemas.end() && definition->second->owner_from_id) return id;
        auto resolve=owner_resolver; return resolve ? resolve(type,id) : std::string{};
    }
    std::shared_ptr<Admission> admit(std::size_t bytes,Reply reply) {
        if(stopped){reply({Status::disconnected,{}});return {};}
        if(draining || admission_serial==UINT64_MAX || counters.pending>=limits.requests || bytes>limits.queued_bytes-counters.queued_bytes) {
            reply({Status::overloaded,{}});return {};
        }
        auto admission=std::make_shared<Admission>(Admission{++admission_serial,bytes,std::move(reply),Clock::now()+limits.request_timeout});
        admissions.emplace(admission->id,admission);++counters.pending;counters.queued_bytes+=bytes;return admission;
    }
    Reply take(const std::shared_ptr<Admission>& admission) {
        if(std::exchange(admission->done,true))return {};
        admissions.erase(admission->id);--counters.pending;counters.queued_bytes-=admission->bytes;return std::move(admission->reply);
    }
    void finish(const std::shared_ptr<Admission>& admission,Result result) {if(auto reply=take(admission))reply(std::move(result));}
    void dispatch(CallIdentity identity,std::string data,Reply reply) {
        auto call=parse_entity_call_view(data);if(!call)return reply({Status::rejected,{}});
        auto id=call->id.str();auto cached=resolved_types.find(id);
        if(cached!=resolved_types.end())return dispatch_resolved(std::move(identity),std::move(data),cached->second,std::move(reply));
        auto admission=admit(data.size(),std::move(reply));if(!admission)return;
        auto input=std::make_shared<std::string>(std::move(data));auto type=std::make_shared<std::string>();
        auto status=std::make_shared<Status>(Status::not_found);
        if(!storage.submit(input->size()+limits.state_bytes,[id,type,status](Database& db){
            auto record=db.get(entity_record_key(EntityId(id)));if(!record)return;
            auto view=decode_entity_record_view(record->value);
            if(!view.metadata || view.metadata->destroyed)return;
            *type=view.metadata->type;*status=Status::ok;
        },[weak=weak_from_this(),admission,input,type,status,identity=std::move(identity)](std::exception_ptr error)mutable{
            auto self=weak.lock();if(!self || admission->done)return;
            if(self->stopped)return self->finish(admission,{Status::disconnected,{}});
            if(Clock::now()>=admission->deadline)return self->finish(admission,{Status::timeout,{}});
            if(error || *status!=Status::ok)return self->finish(admission,{error?Status::internal:*status,{}});
            auto reply=self->take(admission);if(!reply)return;
            if(self->resolved_types.size()>=self->limits.entities)self->resolved_types.clear();
            auto parsed=parse_entity_call_view(*input);self->resolved_types.insert_or_assign(parsed->id.str(),*type);
            self->dispatch_resolved(std::move(identity),std::move(*input),*type,std::move(reply),admission->deadline);
        }))finish(admission,{Status::overloaded,{}});
    }
    void dispatch_resolved(CallIdentity identity,std::string data,const std::string& type,Reply reply,Clock::time_point deadline={}) {
        auto call=parse_entity_call_view(data);if(!call)return reply({Status::rejected,{}});
        if(!call->expected_type.empty() && call->expected_type!=type)return reply({Status::rejected,{}});
        auto route=entity_route(type,call->method);
        enqueue(route,entity_message(call->id.str(),call->payload,call->request_id),std::move(reply),false,std::move(identity),{},0,{},{},deadline);
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
        if(it!=slots.end() && result.status==Status::ok && job->metadata_only && job->previously_owned) {
            // A protocol-only commit may renew this process's lease. Its state
            // and local activation did not change; carry the cache across that
            // renewal exactly as a normal state commit does.
            if(it->second.transient_fence==job->previous_fence) it->second.transient_fence=job->fence;
            if(it->second.activated && it->second.activation_fence==job->previous_fence) it->second.activation_fence=job->fence;
        }
        if (it != slots.end()) { it->second.busy = false; it->second.touched = Clock::now();
            if (result.status == Status::ok && !job->metadata_only) { it->second.activated = true; it->second.activation_fence=job->fence; }
            else if ((!job->fence && !job->preserve_activation) || job->owner_lost) it->second.activated = false;
            it->second.releasing=false; }
        --counters.pending; counters.queued_bytes -= job->request.bytes; counters.state_bytes -= job->resident; job->resident = 0;
        if (result.status == Status::ok) ++counters.committed;
        else if (result.status == Status::rejected) ++counters.rejected;
        else ++counters.failed;
        if (job->request.tick && result.status != Status::ok) ++counters.tick_failures;
        auto reply = std::move(job->request.reply);
        if (job->commit_uncertain) {
            // A storage exception does not prove that the CAS/erase failed.
            // Do not let live replicas or the local transient cache silently
            // retain a pre-commit image. A new generation also invalidates
            // subscriptions if their close notification could not be sent.
            if (it != slots.end()) {
                counters.transient_bytes -= it->second.transient.size();
                std::string{}.swap(it->second.transient);
                it->second.transient_fence = it->second.transient_revision = 0;
                it->second.activated = false;
                it->second.handle.generation = ++generation;
            }
            if (closed_listener) try {
                auto listener = closed_listener; auto split = job->entity.find('\0');
                listener(job->entity.substr(0, split), job->entity.substr(split + 1), false);
            } catch (...) { ++counters.publication_failures; }
        }
        if (result.status == Status::ok && job->attributes && attribute_listener &&
            (job->attributes->previous.values != job->attributes->current.values || job->attributes->previous.revision != job->attributes->current.revision)) {
            try { auto listener = attribute_listener; listener(*job->attributes); } catch (...) { ++counters.publication_failures; }
        }
        if(result.status==Status::ok && is_control(job->request.route,job->request.method->type)) {
            auto found=slots.find(job->entity);
            if(found!=slots.end()) { counters.transient_bytes-=found->second.transient.size(); slots.erase(found); }
            std::erase(schedule,job->entity);
            if(closed_listener) try { auto listener=closed_listener; auto split=job->entity.find('\0'); listener(job->entity.substr(0,split),job->entity.substr(split+1),job->request.route==job->request.method->type+".$destroy"); } catch(...) { ++counters.publication_failures; }
        }
        if(job->wrong_node) {
            auto found=slots.find(job->entity);
            if(found!=slots.end() && found->second.queue.empty()) {
                counters.transient_bytes-=found->second.transient.size(); slots.erase(found); std::erase(schedule,job->entity);
                if(closed_listener) try { auto split=job->entity.find('\0'); auto listener=closed_listener; listener(job->entity.substr(0,split),job->entity.substr(split+1),false); } catch(...) { ++counters.publication_failures; }
            }
        }
        reply(std::move(result));
    }
    static bool is_control(const std::string& route,const std::string& type) {
        return route==type+".$deactivate" || route==type+".$destroy";
    }
    static bool internal(const std::string& route,const std::string& type) {
        return route==type+".$tick" || route==type+".$create" || route==type+".$activate" || is_control(route,type);
    }
    void execute(const std::shared_ptr<Job>& job,Reply done) {
        if(job->metadata_only) return done({Status::ok,state_message(job->state,"")});
        struct Chain { std::vector<std::shared_ptr<Method>> steps; std::size_t index=0; std::string state; Reply done; };
        auto chain=std::make_shared<Chain>(); chain->state=std::move(job->state); chain->done=std::move(done);
        auto type=job->request.method->type;
        if(!is_control(job->request.route,type)) {
            if(job->created && methods.contains(type+".$create")) chain->steps.push_back(methods.at(type+".$create"));
            if(job->activating && methods.contains(type+".$activate")) chain->steps.push_back(methods.at(type+".$activate"));
        }
        auto slot=slots.find(job->entity);
        if(is_control(job->request.route,type) && slot!=slots.end() && !slot->second.activated) {
            auto noop=std::make_shared<Method>(*job->request.method);
            noop->logic=[](const CallIdentity&,std::string input,Reply reply) {
                auto entity=parse_entity_message_view(input); auto state=entity?parse_state_message_view(entity->payload):std::nullopt;
                if(!state) return reply({Status::internal,{}});
                reply({Status::ok,state_message(state->state,"")});
            };
            chain->steps.push_back(std::move(noop));
        } else chain->steps.push_back(job->request.method);
        auto advance=std::make_shared<std::function<void()>>();
        *advance=[weak=weak_from_this(),job,chain,weak_advance=std::weak_ptr(advance)] {
            auto self=weak.lock(); if(!self || self->stopped || job->responded) return;
            if(job->request.deadline<=Clock::now()) return chain->done({Status::timeout,{}});
            auto keep=weak_advance.lock(); if(!keep) return;
            bool last=chain->index+1==chain->steps.size();
            auto method=chain->steps[chain->index++];
            auto identity=last?job->request.identity:CallIdentity{Caller::server,"framework:lifecycle"};
            auto slot=self->slots.find(job->entity); if(slot==self->slots.end()) return chain->done({Status::disconnected,{}});
            auto input=entity_message(slot->second.handle.id,state_message(chain->state,last?std::string_view(job->request.payload):std::string_view{}));
            auto once=std::make_shared<bool>(false);
            auto complete=[weak,job,chain,keep,last,once](Result result) {
                if(std::exchange(*once,true)) return;
                auto self=weak.lock(); if(!self || self->stopped || job->responded) return;
                if(last || result.status!=Status::ok) return chain->done(std::move(result));
                auto state=parse_state_message(result.data);
                if(!state || state->state.size()>self->limits.state_bytes) return chain->done({Status::overloaded,{}});
                auto growth=state->state.size()>chain->state.size()?5*(state->state.size()-chain->state.size()):0;
                if(growth>self->limits.resident_bytes-self->counters.state_bytes) return chain->done({Status::overloaded,{}});
                self->counters.state_bytes+=growth; job->resident+=growth;
                chain->state=std::move(state->state); (*keep)();
            };
            if(last) job->logic_started=true;
            try { method->logic(identity,std::move(input),complete); } catch(...) { complete({Status::internal,{}}); }
        };
        (*advance)();
    }
    void enqueue(const std::string& route, std::string data, Reply reply, bool tick = false, CallIdentity identity = {Caller::server, {}}, std::string handoff_target={}, std::uint64_t accept_transfer=0, std::optional<DeliveryEnvelope> incoming={}, std::optional<DeliveryAck> ack={}, Clock::time_point deadline={}) {
        if (stopped) return reply({Status::disconnected, {}});
        auto method = methods.find(route);
        if (method == methods.end()) return reply({Status::not_found, {}});
        auto selected = method->second;
        auto charged=data.size()+(incoming?encode_delivery(*incoming).size():0)+(ack?ack->result.data.size()+128:0);
        if(draining && !is_control(route,selected->type)) return reply({Status::overloaded,{}});
        if (counters.pending >= limits.requests || charged > limits.queued_bytes - counters.queued_bytes)
            return reply({Status::overloaded, {}});
        auto message = parse_entity_message(data);
        if (!message) return reply({Status::rejected, {}});
        if (!message->request_id.empty() && message->payload.size() + route.size() + message->request_id.size() + 16 > limits.receipt_bytes)
            return reply({Status::overloaded, {}});
        if (selected->policy) {
            try {
                auto policy = *selected->policy;
                if(valid_entity_id(message->id))policy.owner_only=false; // authoritative account is checked after acquisition
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
            it = slots.emplace(entity, Slot{{selected->type, message->id, owner, ++generation}, {}, false, false, false, 0,
                time + limits.tick_interval, time, {}, 0, 0}).first;
            schedule.push_back(entity);
        }
        if (it->second.releasing) return reply({Status::rejected, {}});
        auto bytes = charged;
        it->second.queue.push_back({selected, std::move(message->payload), route, std::move(message->request_id), std::move(reply),
            Clock::now() + limits.request_timeout, bytes, tick, std::move(identity), std::move(handoff_target), accept_transfer, std::move(incoming), std::move(ack), {}});
        auto& request=it->second.queue.back();
        if(deadline!=Clock::time_point{})request.deadline=std::min(request.deadline,deadline);
        if(request.incoming) {
            const auto& m=request.incoming->message; auto found=methods.find(selected->type+"."+m.method);
            if(found==methods.end() || internal(found->first,selected->type)) request.delivery_error=Result{Status::not_found,{}};
            else { request.method=found->second; request.route=found->first; }
        }
        ++counters.pending; counters.queued_bytes += bytes;
        if(is_control(request.route,selected->type)) it->second.releasing=true;
    }
    void acquire(const std::string& entity, Slot& slot) {
        auto job = std::make_shared<Job>(); job->entity = entity;
        job->request = std::move(slot.queue.front()); slot.queue.pop_front(); slot.busy = true;
        job->preserve_activation=job->request.incoming.has_value() || job->request.ack.has_value();
        active.emplace(entity, job);
        auto owner_id = owner; auto clock = now; auto settings = limits;
        auto accepted = storage.submit(job->request.bytes + limits.state_bytes + limits.receipt_bytes + limits.delivery_bytes,
            [job, owner_id, clock, settings](Database& db) {
                for (int attempt = 0; attempt < 4; ++attempt) {
                    auto record = db.get(record_key(job->entity));
                    EntityRecord current;
                    if (record) current = decode_entity_record(record->value);
                    else {
                        auto split=job->entity.find('\0');
                        if(valid_entity_id(std::string_view(job->entity).substr(split+1))) {job->status=Status::not_found;return;}
                        if(job->request.incoming || job->request.ack) { job->status=Status::not_found; return; }
                        current.initialized=false;
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
                    if(current.metadata && (current.metadata->destroyed || current.metadata->type!=job->request.method->type)) {
                        job->status=current.metadata->destroyed?Status::not_found:Status::rejected;return;
                    }
                    auto identity_split=job->entity.find('\0');
                    if(valid_entity_id(std::string_view(job->entity).substr(identity_split+1)) && !current.metadata) {
                        job->status=Status::rejected;return;
                    }
                    job->metadata=current.metadata;
                    if(current.metadata && current.metadata->rebuildable &&
                       (job->request.incoming || job->request.ack || !job->request.handoff_target.empty() || job->request.accept_transfer)) {
                        job->status=Status::rejected;return;
                    }
                    if (!current.placement.empty() && !entity_node_matches(owner_id,current.placement)) {
                        job->owner_lost=true; job->wrong_node=true;
                        job->status=(!job->request.accept_transfer && job->request.route==job->request.method->type+".$snapshot")?Status::overloaded:Status::rejected; return;
                    }
                    if (job->request.accept_transfer && (!record || current.transfer_id!=job->request.accept_transfer || current.placement.empty())) {
                        job->preserve_activation=true; job->status=Status::rejected; return;
                    }
                    job->placement=current.placement; job->transfer_id=current.transfer_id;
                    job->created=!current.initialized;
                    auto time = clock();
                    if (time < 0 || time > std::numeric_limits<std::int64_t>::max() - settings.lease.count())
                        throw std::runtime_error("invalid lease clock");
                    if (current.owner != owner_id && current.expires_ms > time) {
                        job->owner_lost=true; job->wrong_node=true;
                        job->status=(!job->request.accept_transfer && job->request.route==job->request.method->type+".$snapshot")?Status::overloaded:Status::rejected; return;
                    }
                    if(current.metadata && current.metadata->rebuildable && current.owner!=owner_id) {
                        current.snapshot.state=current.metadata->initial_state;current.snapshot.receipts.clear();
                    }
                    job->delivery=current.snapshot.delivery;
                    job->metadata_only=job->request.ack.has_value() || job->request.delivery_error.has_value();
                    if(job->request.incoming) {
                        const auto& incoming=*job->request.incoming;const auto& m=incoming.message;
                        if(!current.initialized || job->delivery.epoch!=m.target_epoch) { job->status=Status::not_found; job->preserve_activation=true; return; }
                        auto channel=delivery_channel(incoming.source_type,incoming.source_id,incoming.source_epoch);
                        auto found=job->delivery.inbox.find(channel);
                        if(found!=job->delivery.inbox.end() && m.sequence<=found->second.sequence) {
                            if(m.sequence==found->second.sequence && found->second.digest!=delivery_digest(incoming)) { job->status=Status::rejected; job->preserve_activation=true; return; }
                            job->replay=true;job->metadata_only=true;
                            job->output=encode_delivery_ack(m.ticket,m.sequence==found->second.sequence?found->second.result:Result{Status::ok,{}});
                        } else {
                            auto expected=found==job->delivery.inbox.end()?1:found->second.sequence+1;
                            if(m.sequence!=expected || (found==job->delivery.inbox.end() && job->delivery.inbox.size()>=settings.delivery_channels)) { job->status=Status::overloaded;return; }
                            auto source=db.get(entity_record_key(incoming.source_type,incoming.source_id));
                            if(!source) { job->status=Status::rejected;return; }
                            auto source_record=decode_entity_record(source->value);const auto& state=source_record.snapshot.delivery;
                            bool valid=false;
                            if(source_record.initialized && state.epoch==incoming.source_epoch) for(const auto& pending:state.outbox)
                                if(!pending.done && pending.ticket==m.ticket && encode_delivery({incoming.source_type,incoming.source_id,incoming.source_epoch,pending})==encode_delivery(incoming)) { valid=true;break; }
                            if(!valid) { job->status=Status::rejected;return; }
                        }
                    }
                    if(job->request.ack) {
                        const auto& ack=*job->request.ack;
                        if(job->delivery.epoch!=ack.epoch) { job->status=Status::not_found;return; }
                        for(auto& m:job->delivery.outbox) if(m.ticket==ack.ticket) {
                            if(m.done && (m.result.status!=ack.result.status || m.result.data!=ack.result.data)) {job->status=Status::rejected;return;}
                            m.done=true;m.result=ack.result;break;
                        }
                    }
                    if(job->request.route==job->request.method->type+".$destroy")
                        for(const auto& m:job->delivery.outbox) if(!m.done) {job->preserve_activation=true;job->status=Status::overloaded;return;}
                    job->state = current.snapshot.state; job->receipts = current.snapshot.receipts;
                    job->previously_owned = current.owner == owner_id; job->previous_fence = current.fence;
                    if (!job->request.request_id.empty()) for (const auto& receipt : job->receipts) {
                        if (receipt.id != job->request.request_id) continue;
                        if (receipt.route != job->request.route || receipt.request != job->request.payload) { job->status = Status::rejected; return; }
                        job->replay = true; job->metadata_only = true; job->output = receipt.response; break;
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
                if (job->status != Status::ok) {
                    if(job->request.incoming && job->status==Status::not_found) {job->metadata_only=true;return self->complete(job,{Status::ok,encode_delivery_ack(job->request.incoming->message.ticket,{Status::not_found,{}})});}
                    return self->complete(job, {job->status, {}});
                }
                if(job->metadata) {
                    auto slot=self->slots.find(job->entity);if(slot!=self->slots.end())slot->second.account=job->metadata->owner_account;
                }
                if(self->delivery_enabled && !job->delivery.epoch) job->delivery.epoch=job->version;
                if (job->request.deadline <= Clock::now()) return self->complete(job, {Status::timeout, {}});
                if(job->request.incoming && !job->replay && job->request.method->policy) {
                    try {
                        auto slot=self->slots.find(job->entity);
                        auto account=job->request.method->policy->owner_only && slot!=self->slots.end()?self->account(slot->second.handle.type,slot->second.handle.id):std::string{};
                        if(!job->request.method->policy->allows(job->request.identity,account)) {job->request.delivery_error=Result{Status::rejected,{}};job->metadata_only=true;}
                    } catch(...) {return self->complete(job,{Status::internal,{}});}
                }
                // Recheck caller roles as well as account ownership: a queued
                // or suspended call may have lost either authorization.
                if (!job->request.incoming && job->request.method->policy) {
                    try {
                        auto current_slot = self->slots.find(job->entity);
                        if (current_slot == self->slots.end()) return self->complete(job, {Status::disconnected, {}});
                        auto account = job->request.method->policy->owner_only ? self->account(current_slot->second.handle.type, current_slot->second.handle.id) : std::string{};
                        if (!job->request.method->policy->allows(job->request.identity, account)) return self->complete(job, {Status::rejected, {}});
                    } catch (...) { return self->complete(job, {Status::rejected, {}}); }
                }
                if (job->replay) { ++self->counters.replayed; return self->complete(job, {Status::ok, std::move(job->output)}); }
                std::size_t retained = job->state.size() + (job->delivery.epoch?2*encode_delivery_state(job->delivery).size():0);
                for (const auto& receipt : job->receipts) retained += receipt.bytes();
                if (retained > self->limits.resident_bytes - self->counters.state_bytes)
                    return self->complete(job, {Status::overloaded, {}});
                job->resident = retained; self->counters.state_bytes += retained;
                auto slot = self->slots.find(job->entity);
                if (slot == self->slots.end()) return self->complete(job, {Status::disconnected, {}});
                if (job->request.method->schema && !job->metadata_only) {
                    try {
                        auto image = decode_attributes(job->state);
                        if (!image.snapshot) throw std::runtime_error("persistent properties require a snapshot");
                        EntityAttributes model(*job->request.method->schema, "");
                        if(job->metadata && (job->created || job->metadata->rebuildable))model.restore_full(image.values,image.revision);
                        else model.restore(image.values, image.revision);
                        auto& cached = slot->second;
                        if (!cached.transient.empty() && job->previously_owned && cached.transient_fence == job->previous_fence &&
                            (cached.transient_revision == image.revision || (job->metadata && job->metadata->rebuildable))) {
                            auto transient = decode_attributes(cached.transient);
                            model.restore_full(transient.values,(job->metadata && job->metadata->rebuildable)?transient.revision:image.revision);
                        }
                        if(job->created && job->request.route==job->request.method->type+".$spawn") {
                            auto requested=decode_attributes(job->request.payload).values;
                            EntityAttributes::Values transient;
                            for(const auto& [name,value]:requested)
                                if(!model.schema().attributes.at(name).persist) transient.emplace(name,value);
                            model.restore_full(transient,image.revision);
                        }
                        AttributeUpdate full{0, model.revision(), true, model.all_values()};
                        auto bytes = encode_attributes(full);
                        auto retained = job->resident + 4 * bytes.size() + 65536 + 4 * attribute_image_bytes(full.values);
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
                        EntityAttributes model(*job->attributes->schema, job->attributes->owner);
                        model.restore_full(job->attributes->previous.values, job->attributes->previous.revision);
                        (void)stage_attribute_proposal(model,proposal,job->request.identity);
                    } catch (...) { return self->complete(job, {Status::rejected, {}}); }
                }
                auto current_slot = self->slots.find(job->entity);
                if (self->stopped || job->responded || current_slot == self->slots.end()) return;
                job->activating=!current_slot->second.activated || !job->previously_owned || current_slot->second.activation_fence!=job->previous_fence;
                if(job->request.incoming && !job->metadata_only) {
                    auto retained=job->state.size()+2*encode_delivery_state(job->delivery).size();
                    if(retained>self->limits.resident_bytes-self->counters.state_bytes) return self->complete(job,{Status::overloaded,{}});
                    job->rollback=job->state;job->delivery_rollback=job->delivery;job->resident+=retained;self->counters.state_bytes+=retained;
                }
                auto done = [weak, job](Result result) {
                    auto owner = weak.lock();
                    if (!owner || owner->stopped || job->responded || job->committing) return;
                    job->committing = true;
                    if (result.status != Status::ok) {
                        if(job->request.incoming && job->logic_started && (result.status==Status::rejected || result.status==Status::not_found)) {
                            job->delivery_result=std::move(result);job->metadata_only=true;job->delivery=std::move(*job->delivery_rollback);result={Status::ok,state_message(job->rollback,"")};
                        } else return owner->complete(job, std::move(result));
                    }
                    if (job->request.deadline <= Clock::now()) return owner->complete(job, {Status::timeout, {}});
                    if (!job->metadata_only && job->request.method->policy) {
                        try {
                            auto slot = owner->slots.find(job->entity);
                            auto account = job->request.method->policy->owner_only && slot != owner->slots.end() ? owner->account(slot->second.handle.type, slot->second.handle.id) : std::string{};
                            if (!job->request.method->policy->allows(job->request.identity, account)) return owner->complete(job, {Status::rejected, {}});
                        } catch (...) { return owner->complete(job, {Status::rejected, {}}); }
                    }
                    auto next = parse_state_message(result.data);
                    if (!next) return owner->complete(job, {Status::internal, {}});
                    std::string{}.swap(job->rollback);job->delivery_rollback.reset();
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
                                if (!definition.persist || (job->metadata && job->metadata->rebuildable)) transient.emplace(name, model.all_values().at(name));
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
                    if (job->attributes) {
                        auto found = owner->slots.find(job->entity);
                        if (found == owner->slots.end()) return owner->complete(job, {Status::disconnected, {}});
                        auto previous = found->second.transient.size();
                        auto growth = job->transient.size() > previous ? job->transient.size() - previous : 0;
                        if (growth > owner->limits.resident_bytes - owner->counters.transient_bytes)
                            return owner->complete(job, {Status::overloaded, {}});
                        owner->counters.transient_bytes += growth; job->cache_growth = growth;
                    }
                    if(job->request.incoming) {
                        const auto& incoming=*job->request.incoming;
                        const auto* outcome=job->request.delivery_error?&*job->request.delivery_error:(job->delivery_result?&*job->delivery_result:nullptr);
                        const auto& response=outcome?outcome->data:job->output;
                        if(response.size()>delivery_payload_limit) return owner->complete(job,{Status::overloaded,{}});
                        Result value{outcome?outcome->status:Status::ok,response};
                        job->delivery.inbox[delivery_channel(incoming.source_type,incoming.source_id,incoming.source_epoch)]={incoming.message.sequence,delivery_digest(incoming),value};
                        job->output=encode_delivery_ack(incoming.message.ticket,value);
                    }
                    // Destruction hooks may have staged new commands since
                    // acquisition. They must not be erased by the same destroy.
                    if(job->request.route==job->request.method->type+".$destroy")
                        for(const auto& m:job->delivery.outbox) if(!m.done) return owner->complete(job,{Status::overloaded,{}});
                    try {
                        if(job->delivery.epoch && (job->delivery.outbox.size()>owner->limits.delivery_messages || job->delivery.inbox.size()>owner->limits.delivery_channels ||
                           job->delivery.sent.size()>owner->limits.delivery_channels || delivery_charge(job->delivery)>owner->limits.delivery_bytes)) return owner->complete(job,{Status::overloaded,{}});
                    } catch(...) {return owner->complete(job,{Status::overloaded,{}});}
                    // Count the new inbox/result and the encoded ACK, not just
                    // the pre-delivery protocol image.
                    const auto retained = (job->attributes ? 4*attribute_image_bytes(job->attributes->current.values)+4*attribute_image_bytes(job->attributes->previous.values) : 0) + (job->attributes ? 5 : 1) * job->state.size() + job->transient.size() * 4 + job->output.size() + receipt_bytes + 12 + (job->delivery.epoch?2*encode_delivery_state(job->delivery).size():0);
                    if (retained > owner->limits.resident_bytes - owner->counters.state_bytes + job->resident)
                        return owner->complete(job, {Status::overloaded, {}});
                    owner->counters.state_bytes = owner->counters.state_bytes - job->resident + retained;
                    job->resident = retained;
                    bool release=Impl::is_control(job->request.route,job->request.method->type);
                    EntityRecord committed{release?std::string{}:owner->owner,release?0:job->expires,job->fence,
                        {std::move(job->state),std::move(job->receipts),std::move(job->delivery)},release?!job->created:true,
                        job->request.handoff_target.empty()?job->placement:job->request.handoff_target,
                        job->request.handoff_target.empty()?job->transfer_id:job->version,job->metadata};
                    if(committed.metadata && committed.metadata->rebuildable)committed.snapshot.state=committed.metadata->initial_state;
                    if(committed.metadata && job->request.route==job->request.method->type+".$destroy") {
                        committed.metadata->destroyed=true;committed.snapshot={};
                    }
                    job->persisted = encode_entity_record(committed);
                    if (!job->request.handoff_target.empty()) { job->output.clear(); entity_record_detail::integer(job->output,job->version); }
                    auto clock = owner->now;
                    bool accepted = owner->storage.submit(job->persisted.size() + job->output.size(),
                        [job, clock](Database& db) {
                            if (clock() >= job->expires) { job->owner_lost=true; job->status = Status::rejected; return; }
                            bool destroy=job->request.route==job->request.method->type+".$destroy";
                            bool accepted=destroy && !job->metadata?db.erase(record_key(job->entity),job->version):db.compare_exchange(record_key(job->entity),job->version,job->persisted);
                            if(!accepted) { job->owner_lost=true; job->status=Status::rejected; }
                        }, [weak, job](std::exception_ptr error) {
                            auto owner = weak.lock();
                            if (!owner || owner->stopped || job->responded) return;
                            if (error) { job->commit_uncertain = true; owner->complete(job, {Status::internal, {}}); }
                            else owner->complete(job, {job->status, job->status == Status::ok ? std::move(job->output) : std::string{}});
                        });
                    if (!accepted) owner->complete(job, {Status::overloaded, {}});
                };
                try { self->execute(job,done); }
                catch (...) { done({Status::internal, {}}); }
            });
        if (!accepted) {
            // The request was admitted to the mailbox. Temporary executor
            // pressure defers acquisition, preserving its original deadline.
            active.erase(entity); slot.busy=false;
            slot.queue.push_front(std::move(job->request));
        }
    }
};
EntityScriptHost::EntityScriptHost(std::shared_ptr<ScriptHost> parent, std::shared_ptr<Database> db,
    std::string owner, EntityRuntimeLimits limits, StorageLimits storage, WallClock now)
  : impl_(std::make_shared<Impl>(std::move(parent), std::move(db), std::move(owner), limits, storage, std::move(now))) {}
EntityScriptHost::~EntityScriptHost() { stop(); }
bool EntityScriptHost::has_route(const std::string& route) const { return impl_->methods.contains(route) || impl_->parent->has_route(route); }
void EntityScriptHost::rpc_policy(const std::string& route, RpcPolicy policy) {
    auto found = impl_->methods.find(route);
    if (found == impl_->methods.end()) return impl_->parent->rpc_policy(route, policy);
    if (!found->second->policy)
        throw std::invalid_argument("policy update requires a contextual entity RPC");
    auto transport = policy; transport.owner_only = false;
    // Validate/update the public entry point first. Failed updates leave the
    // entity policy unchanged. Every queued request shares this Method.
    impl_->parent->rpc_policy(route, transport);
    found->second->policy = policy;
}
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
    // Delivery metadata is independent of typed attributes. Opaque native
    // entities need the same private receive/ACK entry as schema entities.
    auto metadata=method->type+".$snapshot";
    bool added_metadata=false;
    if(!self.methods.contains(metadata)) {
        auto snapshot=std::make_shared<Impl::Method>(Impl::Method{method->type,method->initial,
            [](const CallIdentity&,std::string input,Reply reply) {
                auto entity=parse_entity_message_view(input);
                auto state=entity?parse_state_message_view(entity->payload):std::nullopt;
                if(!state)return reply({Status::internal,{}});
                reply({Status::ok,state_message(state->state,state->state)});
            },RpcPolicy{false,true,false},method->schema});
        self.methods.emplace(metadata,std::move(snapshot));added_metadata=true;
    }
    if (Impl::internal(route,method->type)) return; // runtime-only; clients cannot drive ticks
    try {
        self.parent->bind(route, [weak = std::weak_ptr(impl_), route](std::string data, Reply reply) {
            auto self = weak.lock();
            if (!self) return reply({Status::disconnected, {}});
            self->enqueue(route, std::move(data), std::move(reply));
        });
    } catch (...) { self.methods.erase(route); if(added_metadata)self.methods.erase(metadata); throw; }
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
                auto entity = parse_entity_message_view(input);
                auto state = entity ? parse_state_message_view(entity->payload) : std::nullopt;
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
void EntityScriptHost::enable_global_entities() {
    if(impl_->global_entry)return;
    auto weak=std::weak_ptr(impl_);
    impl_->parent->bind_context(entity_call_route,{true,true,false},[weak](const CallIdentity& who,std::string data,Reply reply){
        if(auto self=weak.lock())self->dispatch(who,std::move(data),std::move(reply));else reply({Status::disconnected,{}});
    });
    impl_->global_entry=true;
}
void EntityScriptHost::on_create_on_node(CreateOnNode fn) {impl_->create_on_node=std::move(fn);}
void EntityScriptHost::create_entity(std::string type,EntityAttributes::Values values,EntitySpawnOptions options,Reply reply) {
    auto keep=impl_;auto& self=*keep;
    std::string state;
    auto definition=schema(type);if(!definition)return reply({Status::not_found,{}});
    try {
        EntityAttributes model(*definition,options.owner_account_id);model.publish(model.stage(values,{Caller::server,{}}));
        state=encode_attributes({0,model.revision(),true,model.all_values()});
        if(state.size()>self.limits.state_bytes || options.scope.empty() || options.scope.size()>256 ||
           options.creation_key.empty() || options.creation_key.size()>128 || options.owner_account_id.size()>256 ||
           (!options.target_node.empty() && !valid_entity_node(options.target_node)))throw std::invalid_argument("invalid entity creation options");
        enable_global_entities();
    }catch(...) {return reply({Status::rejected,{}});}
    if(options.target_node.empty())options.target_node=self.owner.substr(0,self.owner.find('/'));
    auto admission=self.admit(state.size()+type.size()+options.scope.size()+options.creation_key.size()+options.owner_account_id.size()+options.target_node.size(),std::move(reply));
    if(!admission)return;
    struct Created {std::string id,type,target;Status status=Status::ok;bool initialized=false;};
    auto created=std::make_shared<Created>();auto catalog=self.catalog;
    if(!self.storage.submit(state.size()*2+2048,[catalog,created,type=std::move(type),state=std::move(state),options=std::move(options)](Database&) {
        try {
            auto id=catalog->create(options.scope,options.creation_key,{type,options.owner_account_id,state,options.target_node,options.persistence});
            auto description=catalog->find(id);created->id=id.str();created->type=type;
            if(!description || description->record.metadata->destroyed){created->status=Status::not_found;return;}
            created->initialized=description->record.initialized;created->target=description->record.placement;
        }catch(const EntityCreationConflict&) {created->status=Status::rejected;}
        catch(const std::invalid_argument&) {created->status=Status::rejected;}
    },[weak=std::weak_ptr(keep),created,admission](std::exception_ptr error) {
        auto self=weak.lock();if(!self || admission->done)return;
        if(error || created->status!=Status::ok)return self->finish(admission,{error?Status::internal:created->status,{}});
        if(self->stopped || Clock::now()>=admission->deadline)return self->finish(admission,{self->stopped?Status::disconnected:Status::timeout,{}});
        if(created->initialized)return self->finish(admission,{Status::ok,created->id});
        auto complete=[weak,created,admission](Result result) {
            if(auto self=weak.lock()){if(result.status==Status::ok)result.data=created->id;self->finish(admission,std::move(result));}
        };
        if(!entity_node_matches(self->owner,created->target)) {
            auto create=self->create_on_node;if(!create)return self->finish(admission,{Status::not_found,{}});
            return create(created->type,created->id,created->target,std::move(complete));
        }
        auto reply=self->take(admission);if(!reply)return;
        self->enqueue(created->type+".$snapshot",entity_message(created->id,""),[created,reply=std::move(reply)](Result result)mutable{
            if(result.status==Status::ok)result.data=created->id;
            reply(std::move(result));
        },false,{Caller::server,"framework:create"},{},0,{},{},admission->deadline);
    }))self.finish(admission,{Status::overloaded,{}});
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
void EntityScriptHost::on_closed(Closed listener) { impl_->closed_listener=std::move(listener); }
void EntityScriptHost::on_attributes(AttributeListener listener) { impl_->attribute_listener = std::move(listener); }
void EntityScriptHost::ownership(OwnerResolver resolve) { impl_->owner_resolver = std::move(resolve); }
void EntityScriptHost::bind_context(std::string route, RpcPolicy policy, ContextHandler handler) {
    impl_->parent->bind_context(std::move(route), policy, std::move(handler));
}
void EntityScriptHost::bind_stateful_context(std::string route, std::string type, std::string initial, RpcPolicy policy, ContextHandler handler) {
    if (!handler || (!policy.client && !policy.server)) throw std::invalid_argument("explicit contextual handler required");
    enable_global_entities();
    bind_stateful(route, std::move(type), std::move(initial), [](std::string, Reply reply) { reply({Status::internal, {}}); });
    auto method = impl_->methods.at(route);
    method->logic = std::move(handler); method->policy = policy;
    if (Impl::internal(route,method->type)) return;
    impl_->parent->unbind(route);
    auto transport_policy = policy; transport_policy.owner_only = false;
    try {
        impl_->parent->bind_context(route, transport_policy,
            [weak = std::weak_ptr(impl_), route](const CallIdentity& identity, std::string data, Reply reply) {
                auto self = weak.lock();
                if (!self) return reply({Status::disconnected, {}});
                self->enqueue(route, std::move(data), std::move(reply), false, identity);
            });
    } catch (...) { unbind(route); throw; }
}
void EntityScriptHost::unbind(const std::string& route) {
    impl_->parent->unbind(route);
    auto found=impl_->methods.find(route);if(found==impl_->methods.end())return;
    auto type=found->second->type;impl_->methods.erase(found);
    if(!impl_->schemas.contains(type)) {
        auto metadata=type+".$snapshot";
        bool remaining=false;for(const auto& [name,method]:impl_->methods)if(method->type==type&&name!=metadata){remaining=true;break;}
        if(!remaining)impl_->methods.erase(metadata);
    }
}
void EntityScriptHost::call_entity(std::string route, std::string data, std::chrono::milliseconds timeout, Reply reply) {
    if(route==entity_call_route) {
        if(timeout.count()<=0)return reply({Status::rejected,{}});
        return impl_->dispatch({Caller::server,"script:"+impl_->owner},std::move(data),std::move(reply));
    }
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
    std::vector<std::shared_ptr<Impl::Admission>> expired;
    for(const auto& [id,admission]:self.admissions) {(void)id;if(Clock::now()>=admission->deadline)expired.push_back(admission);}
    for(auto& admission:expired)self.finish(admission,{Status::timeout,{}});
    auto time = Clock::now();
    auto remaining = self.schedule.size();
    while (remaining-- && !self.schedule.empty()) {
        if (work >= budget || self.stopped) break;
        auto entity = std::move(self.schedule.front()); self.schedule.pop_front();
        auto it = self.slots.find(entity);
        if (it == self.slots.end()) continue;
        auto& slot = it->second;
        if (!slot.busy && slot.queue.empty() && time - slot.touched >= self.limits.idle_timeout) {
            auto route=slot.handle.type+".$deactivate";
            if(self.methods.contains(route)) { self.enqueue(route,entity_message(slot.handle.id,"idle"),[](Result){}); }
            else { self.counters.transient_bytes-=slot.transient.size(); self.slots.erase(it); continue; }
        }
        self.schedule.push_back(entity);
        if (slot.busy) continue;
        if (slot.queue.empty() && time >= slot.next_tick) {
            auto route = slot.handle.type + ".$tick";
            if (!self.draining && slot.activated && self.methods.contains(route)) self.enqueue(route, entity_message(slot.handle.id,
                std::to_string(self.limits.tick_interval.count())), [](Result) {}, true);
            slot.next_tick = time + self.limits.tick_interval;
        }
        if (slot.queue.empty()) continue;
        if (slot.queue.front().deadline <= time) {
            auto request = std::move(slot.queue.front()); slot.queue.pop_front();
            if(Impl::is_control(request.route,request.method->type))slot.releasing=false;
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
    if(self.methods.contains(handle.type+".$deactivate")) {
        self.enqueue(handle.type+".$deactivate",entity_message(handle.id,"explicit"),std::move(reply)); return;
    }
    it->second.busy = true; it->second.releasing = true;
    auto release = std::make_shared<Impl::Release>(Impl::Release{std::move(reply)});
    self.releases.emplace(entity, release);
    auto status = std::make_shared<Status>(Status::rejected); auto id = self.owner;
    auto done = [weak = std::weak_ptr(impl_), entity, status, release](std::exception_ptr error) {
        if (std::exchange(release->done, true)) return;
        auto owner = weak.lock();
        if (!owner || owner->stopped) return release->reply({Status::disconnected, {}});
        owner->releases.erase(entity);
        if (!error && *status == Status::ok) { auto it = owner->slots.find(entity); if (it != owner->slots.end()) owner->counters.transient_bytes -= it->second.transient.size(); owner->slots.erase(entity); std::erase(owner->schedule, entity); if(owner->closed_listener) try { auto split=entity.find('\0'); owner->closed_listener(entity.substr(0,split),entity.substr(split+1),false); } catch(...) { ++owner->counters.publication_failures; } }
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
const std::string& EntityScriptHost::owner_id() const { return impl_->owner; }
bool EntityScriptHost::accepts_handoff(const std::string& type,const std::string& id) const {
    return !impl_->stopped && !impl_->draining && impl_->schemas.contains(type) &&
        (impl_->slots.contains(key(type,id)) || impl_->slots.size()<impl_->limits.entities);
}
void EntityScriptHost::on_transfer(Transfer fn) { impl_->transfer=std::move(fn); }
void EntityScriptHost::enable_delivery(bool enabled) { impl_->delivery_enabled=enabled; }
void EntityScriptHost::send_entity(std::string source_type,std::string source_id,std::string type,std::string id,std::string method,std::string payload,Reply reply) {
    auto keep=impl_;auto& self=*keep;
    auto found=self.active.find(key(source_type,source_id));
    if(self.stopped || !self.delivery_enabled || found==self.active.end() || found->second->responded || found->second->committing)
        return reply({Status::rejected,{}});
    auto job=found->second;
    if(job->metadata && job->metadata->rebuildable)return reply({Status::rejected,{}});
    std::optional<Status> invalid;
    try {
        (void)entity_route(type.empty() && valid_entity_id(id)?"_":type,method);(void)entity_message(id,"");
        if(method.front()=='$' || payload.size()>delivery_payload_limit || job->delivery.outbox.size()>=self.limits.delivery_messages)
            invalid=Status::overloaded;
    } catch(...) {invalid=Status::rejected;}
    if(invalid) return reply({*invalid,{}});
    auto target_epoch=std::make_shared<std::uint64_t>(0);auto status=std::make_shared<Status>(Status::overloaded);
    auto source=std::make_pair(source_type,source_id);auto settings=self.limits;
    auto target_type=type,target_id=id;auto queued_bytes=payload.size()+1024;
    auto resolved_type=std::make_shared<std::string>(type);
    if(!self.storage.submit(queued_bytes,[source,target_type,target_id,target_epoch,status,settings,resolved_type](Database& db) {
        auto target=db.get(entity_record_key(target_type,target_id));
        if(!target) {*status=Status::not_found;return;}
        auto value=decode_entity_record(target->value);
        if(value.metadata && (value.metadata->destroyed || value.metadata->rebuildable || (!target_type.empty() && value.metadata->type!=target_type))) {*status=Status::rejected;return;}
        if(value.metadata)*resolved_type=value.metadata->type;
        if(!value.initialized || !value.snapshot.delivery.epoch) {*status=Status::overloaded;return;}
        *target_epoch=value.snapshot.delivery.epoch;
        // Register before the source CAS. A crash may leave an empty registry
        // entry, never an accepted outbox that a restarted worker cannot find.
        for(unsigned attempt=0;attempt<8;++attempt) {
            auto record=db.get(delivery_registry_key);
            auto sources=record?decode_delivery_sources(record->value):std::vector<std::pair<std::string,std::string>>{};
            if(std::find(sources.begin(),sources.end(),source)!=sources.end()) {*status=Status::ok;return;}
            if(sources.size()>=settings.delivery_sources) return;
            sources.push_back(source);
            if(db.compare_exchange(delivery_registry_key,record?record->version:0,encode_delivery_sources(sources))) {*status=Status::ok;return;}
        }
    },[weak=std::weak_ptr(keep),job,id=std::move(id),method=std::move(method),payload=std::move(payload),source_type=std::move(source_type),source_id=std::move(source_id),target_epoch,status,reply,resolved_type](std::exception_ptr error) mutable {
        auto self=weak.lock();if(!self || self->stopped || job->responded || job->committing) return reply({Status::disconnected,{}});
        if(error) return reply({Status::internal,{}});
        if(*status!=Status::ok) return reply({*status,{}});
        Result prepared{Status::overloaded,{}};
        try { prepared = [&]() -> Result {
            auto type=*resolved_type;auto next=job->delivery;auto channel=delivery_channel(type,id,*target_epoch);
            if(!next.sent.contains(channel) && next.sent.size()>=self->limits.delivery_channels) return {Status::overloaded,{}};
            auto& sequence=next.sent[channel];if(sequence==UINT64_MAX) return {Status::overloaded,{}};
            DeliveryMessage message;message.type=std::move(type);message.id=std::move(id);message.method=std::move(method);message.payload=std::move(payload);
            message.target_epoch=*target_epoch;message.sequence=++sequence;
            message.ticket=delivery_ticket({source_type,source_id,next.epoch,message});auto ticket=message.ticket;
            next.outbox.push_back(std::move(message));
            if(delivery_charge(next)>self->limits.delivery_bytes || next.outbox.size()>self->limits.delivery_messages) return {Status::overloaded,{}};
            auto previous=encode_delivery_state(job->delivery).size(),bytes=encode_delivery_state(next).size();auto growth=bytes>previous?2*(bytes-previous):0;
            if(growth>self->limits.resident_bytes-self->counters.state_bytes) return {Status::overloaded,{}};
            self->counters.state_bytes+=growth;job->resident+=growth;job->delivery=std::move(next);
            return {Status::ok,std::move(ticket)};
        }(); } catch(...) {}
        // Application callback exceptions are not preparation failures and
        // must never cause the same callback to be invoked a second time.
        reply(std::move(prepared));
    })) reply({Status::overloaded,{}});
}
std::optional<DeliveryMessage> EntityScriptHost::delivery_status(const std::string& type,const std::string& id,const std::string& ticket) const {
    auto found=impl_->active.find(key(type,id));if(found==impl_->active.end() || found->second->responded) return {};
    for(const auto& m:found->second->delivery.outbox) if(m.ticket==ticket) return m;
    return {};
}
bool EntityScriptHost::forget_delivery(const std::string& type,const std::string& id,const std::string& ticket) {
    auto found=impl_->active.find(key(type,id));if(found==impl_->active.end() || found->second->responded || found->second->committing) return false;
    auto& messages=found->second->delivery.outbox;
    auto message=std::find_if(messages.begin(),messages.end(),[&](const auto& m){return m.ticket==ticket && m.done;});
    if(message==messages.end()) return false;
    messages.erase(message);return true;
}
void EntityScriptHost::receive_delivery(CallIdentity who,DeliveryEnvelope envelope,Reply reply) {
    if(who.caller!=Caller::server || !impl_->delivery_enabled || impl_->stopped) return reply({Status::rejected,{}});
    try { (void)decode_delivery(encode_delivery(envelope)); } catch(...) {return reply({Status::rejected,{}});}
    const auto& m=envelope.message;auto route=m.type+".$snapshot";auto data=entity_message(m.id,m.payload);
    impl_->enqueue(route,std::move(data),std::move(reply),false,std::move(who),{},0,std::move(envelope));
}
void EntityScriptHost::acknowledge_delivery(std::string type,std::string id,std::uint64_t epoch,std::string ticket,Result result,Reply reply) {
    if(!impl_->delivery_enabled || impl_->stopped || !epoch || ticket.size()!=64 || result.data.size()>delivery_payload_limit) return reply({Status::rejected,{}});
    auto data=entity_message(id,"");
    impl_->enqueue(type+".$snapshot",std::move(data),std::move(reply),false,{Caller::server,"framework:delivery-ack"},{},0,{},Impl::DeliveryAck{epoch,std::move(ticket),std::move(result)});
}
void EntityScriptHost::transfer_entity(std::string type,std::string id,std::string target,Reply reply) {
    auto fn=impl_->transfer;
    if(impl_->stopped || !fn) return reply({Status::rejected,{}});
    fn(std::move(type),std::move(id),std::move(target),std::move(reply));
}
void EntityScriptHost::accept_handoff(std::string type,std::string id,std::uint64_t ticket,Reply reply) {
    if(!ticket) return reply({Status::rejected,{}});
    std::string message;
    try { message=entity_message(id,""); } catch(...) { return reply({Status::rejected,{}}); }
    impl_->enqueue(type+".$snapshot",std::move(message),[reply=std::move(reply)](Result r) mutable { r.data.clear(); reply(std::move(r)); },
        false,{Caller::server,"framework:handoff"},{},ticket);
}
void EntityScriptHost::handoff(const EntityHandle& handle,std::string target,Reply reply) {
    auto& self=*impl_; auto it=self.slots.find(key(handle.type,handle.id));
    if(self.stopped) return reply({Status::disconnected,{}});
    if(!valid_entity_node(target) || entity_node_matches(self.owner,target) || handle.owner!=self.owner ||
       it==self.slots.end() || it->second.handle.generation!=handle.generation || it->second.busy ||
       it->second.releasing || !it->second.queue.empty() || !self.methods.contains(handle.type+".$deactivate"))
        return reply({Status::rejected,{}});
    self.enqueue(handle.type+".$deactivate",entity_message(handle.id,"transfer"),std::move(reply),false,
        {Caller::server,"framework:handoff"},std::move(target));
}
void EntityScriptHost::control_entity(std::string type,std::string id,bool destroy,Reply reply) {
    auto handle=this->handle(type,id);
    if(!handle) return reply({Status::not_found,{}});
    if(!destroy) return deactivate(*handle,std::move(reply));
    auto& self=*impl_; auto it=self.slots.find(key(type,id));
    if(self.stopped) return reply({Status::disconnected,{}});
    if(it==self.slots.end() || it->second.busy || !it->second.queue.empty() || it->second.releasing || !self.methods.contains(type+".$destroy"))
        return reply({Status::rejected,{}});
    self.enqueue(type+".$destroy",entity_message(id,"destroy"),std::move(reply));
}
void EntityScriptHost::drain(Reply reply) {
    impl_->draining=true;
    auto handles=std::make_shared<std::vector<EntityHandle>>();
    for(const auto& [key,slot]:impl_->slots) { (void)key; if(slot.busy || !slot.queue.empty()) return reply({Status::overloaded,{}}); handles->push_back(slot.handle); }
    if(handles->empty()) return reply({Status::ok,{}});
    struct State { std::size_t pending; Status status=Status::ok; Reply reply; };
    auto state=std::make_shared<State>(State{handles->size(),Status::ok,std::move(reply)});
    for(const auto& h:*handles) deactivate(h,[state](Result r) { if(r.status!=Status::ok) state->status=r.status; if(!--state->pending) state->reply({state->status,{}}); });
}
EntityRuntimeStats EntityScriptHost::stats() const { auto result = impl_->counters; result.entities = impl_->slots.size(); return result; }
void EntityScriptHost::stop() {
    auto keep_alive = impl_; auto& self = *keep_alive;
    if (std::exchange(self.stopped, true)) return;
    // Accepted operations own mutable Job results until the worker has joined.
    // Synchronize before complete() observes or cancels any of those results.
    self.storage.stop();
    if(self.global_entry)self.parent->unbind(entity_call_route);
    auto admissions=std::move(self.admissions);self.admissions.clear();
    for(auto& [id,admission]:admissions) {(void)id;try{self.finish(admission,{Status::disconnected,{}});}catch(...){}}
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
}
}

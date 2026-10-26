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
void validate_entity_endpoints(const std::vector<EntityEndpoint>& endpoints) {
    if(endpoints.empty() || endpoints.size()>1024) throw std::invalid_argument("invalid entity endpoints");
    std::unordered_set<std::string> ids;
    for(const auto& e:endpoints) if(!valid_entity_node(e.node) ||
        e.address.empty() || !e.port || e.types.empty() || !ids.insert(e.node).second) throw std::invalid_argument("invalid entity endpoint");
}
std::optional<std::size_t> select_entity_endpoint(const std::vector<EntityEndpoint>& endpoints,const std::string& type,
    const std::string& id,const std::optional<EntityLocation>& location,std::int64_t time,const std::function<bool(std::size_t)>& eligible) {
    std::optional<std::size_t> selected; std::uint64_t best=0;
    bool owned=location && location->expires_ms>time;
    for(std::size_t i=0;i<endpoints.size();++i) {
        const auto& e=endpoints[i]; if(std::find(e.types.begin(),e.types.end(),type)==e.types.end() || (eligible && !eligible(i))) continue;
        if(owned) { if(entity_node_matches(location->owner,e.node)) return i; }
        else if(location && !location->placement.empty()) { if(location->placement==e.node) return i; }
        else { auto rank=score(type+'\0'+id+'\0'+e.node); if(!selected || rank>best) {selected=i;best=rank;} }
    }
    return selected;
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
    std::vector<EntityEndpoint> endpoints;
    std::unordered_map<std::uint64_t, std::shared_ptr<Call>> calls;
    std::size_t bytes = 0;
    std::uint64_t serial = 0;
    bool stopped = false, polling = false;
    Impl(Node& transport, std::shared_ptr<Storage> database, std::vector<EntityEndpoint> endpoints, EntityRouterLimits settings)
      : node(transport), storage(std::move(database)), limits(settings) {
        validate_entity_endpoints(endpoints);
        if(!limits.requests || !limits.bytes) throw std::invalid_argument("invalid entity router limits");
        this->endpoints=endpoints;
        for(auto& endpoint:endpoints) members.push_back({std::move(endpoint)});
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
        if (!storage.submit(call->type.size() + call->entity.size(), [location, call](Storage& db) {
            *location = entity_location(db, {}, call->entity);
        }, [weak = weak_from_this(), location, call](std::exception_ptr error) {
            auto self = weak.lock();
            if (!self || self->stopped || call->done) return;
            if (Clock::now() >= call->deadline) return self->finish(call, {Status::timeout, {}});
            if (error) return self->finish(call, {Status::internal, {}});
            if(!*location)return self->finish(call,{Status::not_found,{}});
            if(!call->type.empty() && call->type!=(*location)->type)return self->finish(call,{Status::rejected,{}});
            auto selected=select_entity_endpoint(self->endpoints,(*location)->type,call->entity,*location,wall_time(),
                [&](std::size_t i) {return Clock::now()>=self->members[i].retry;});
            if (!selected) return self->failed(call, Status::disconnected);
            auto& member = self->members[*selected];
            if (Clock::now() < member.retry) return self->failed(call, Status::disconnected);
            try {
                // An idle transport may already have closed without a pending
                // RPC to clear this cache. Start a new connection before this
                // new attempt; never replay an unkeyed in-flight failure.
                if (!member.peer || (!self->node.connected(member.peer) && !self->node.connecting(member.peer)))
                    member.peer = self->node.connect(member.endpoint.address, member.endpoint.port, member.endpoint.tls_peer_name);
                auto remaining = std::chrono::ceil<std::chrono::milliseconds>(call->deadline - Clock::now());
                // A slow, healthy RPC is still the original attempt. Retrying it
                // every second cuts off unkeyed calls and needlessly duplicates
                // keyed work. Only actual failures can trigger a safe retry;
                // every attempt shares the caller's original absolute deadline.
                auto attempt_timeout = std::max(remaining, std::chrono::milliseconds(1));
                self->node.call(member.peer, call->route, call->message, attempt_timeout,
                    [weak, call, index = *selected, peer = member.peer](Result result) {
                        auto self = weak.lock();
                        if (!self || self->stopped || call->done) return;
                        if (result.status == Status::disconnected) {
                            auto& member = self->members[index];
                            // A late callback from a replaced connection must
                            // not tear down its healthy replacement.
                            if (member.peer == peer) {
                                member.peer = 0; member.retry = Clock::now() + std::chrono::milliseconds(250);
                                if (peer) self->node.close(peer);
                            }
                        }
                        if (result.status == Status::ok) self->finish(call, std::move(result));
                        else self->failed(call, result.status);
                    });
            } catch (...) { member.retry = Clock::now() + std::chrono::milliseconds(250); self->failed(call, Status::disconnected); }
        })) { call->busy = false; failed(call, Status::overloaded); }
    }
};
EntityRouter::EntityRouter(Node& node, std::shared_ptr<Storage> database, std::vector<EntityEndpoint> endpoints, EntityRouterLimits limits)
  : impl_(std::make_shared<Impl>(node, std::move(database), std::move(endpoints), limits)) {}
EntityRouter::~EntityRouter() { stop(); }
void EntityRouter::bind_owner(std::string type,std::string id,ConnectionRef owner,std::chrono::milliseconds timeout,Reply reply) {
    auto& self=*impl_;if(!reply)throw std::invalid_argument("reply required");
    if(self.stopped)return reply({Status::disconnected,{}});
    if(!valid_entity_id(id) || type.size()>127 || timeout.count()<=0 || timeout>Node::max_call_timeout)return reply({Status::rejected,{}});
    std::string message;
    try{snapshot_detail::blob(message,type);snapshot_detail::blob(message,id);snapshot_detail::blob(message,owner?owner.key():std::string{});}catch(...){return reply({Status::rejected,{}});}
    auto bytes=message.size()+type.size()+id.size();
    if(self.serial==UINT64_MAX || self.calls.size()>=self.limits.requests || bytes>self.limits.bytes-self.bytes)return reply({Status::overloaded,{}});
    auto call=std::make_shared<Impl::Call>();call->id=++self.serial;call->entity=std::move(id);call->type=std::move(type);call->route="system.entity.owner";
    call->message=std::move(message);call->reply=std::move(reply);call->deadline=Clock::now()+timeout;call->bytes=bytes;
    self.calls.emplace(call->id,call);self.bytes+=bytes;
}
void EntityRouter::call(EntityId id,std::string_view method,std::string_view payload,std::chrono::milliseconds timeout,Reply reply,std::string_view request_id,std::string_view expected_type) {
    if(!reply)throw std::invalid_argument("reply required");
    std::string message;
    try{message=entity_call_message(id,method,payload,request_id,expected_type);}catch(...){return reply({Status::rejected,{}});}
    call_encoded(std::move(message),timeout,std::move(reply));
}
void EntityRouter::call_encoded(std::string message,std::chrono::milliseconds timeout,Reply reply) {
    auto& self=*impl_;if(!reply)throw std::invalid_argument("reply required");
    if(self.stopped)return reply({Status::disconnected,{}});
    auto parsed=parse_entity_call_view(message);if(!parsed)return reply({Status::rejected,{}});
    if(timeout.count()<=0 || timeout>Node::max_call_timeout)return reply({Status::rejected,{}});
    auto bytes=message.size()+parsed->expected_type.size()+parsed->request_id.size()+16;
    if(self.serial==UINT64_MAX || self.calls.size()>=self.limits.requests || bytes>self.limits.bytes-self.bytes)return reply({Status::overloaded,{}});
    auto call=std::make_shared<Impl::Call>();call->id=++self.serial;call->entity=parsed->id.str();call->type=parsed->expected_type;call->request_id=parsed->request_id;
    call->route=entity_call_route;call->message=std::move(message);
    call->reply=std::move(reply);call->deadline=Clock::now()+timeout;call->bytes=bytes;self.calls.emplace(call->id,call);self.bytes+=bytes;
}
void EntityRouter::call(std::string type, std::string id, std::string method, std::string payload,
    std::chrono::milliseconds timeout, Reply reply, std::string request_id) {
    if(!reply)throw std::invalid_argument("reply required");
    if(!valid_entity_id(id))return reply({Status::rejected,{}});
    call(EntityId(id),method,payload,timeout,std::move(reply),request_id,type);
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
    self.storage.stop(AsyncDatabase::StopMode::cancel_queued);
}
void RoutedScriptHost::call_entity(std::string route, std::string data, std::chrono::milliseconds timeout, Reply reply) {
    if(route==entity_call_route) {
        return router_.call_encoded(std::move(data),timeout,std::move(reply));
    }
    reply({Status::rejected,{}});
}
}

#include "fsnet/attribute_replication.hpp"
#include <algorithm>
#include <deque>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace fsnet {
namespace {
using Clock = std::chrono::steady_clock;
constexpr const char* channel = "system.attributes.update";
void put(std::string& out, std::uint64_t value, unsigned count) {
    for (unsigned i=count; i; --i) out += static_cast<char>(value >> ((i-1)*8));
}
std::uint64_t get(std::string_view& in, unsigned count) {
    if (in.size() < count) throw std::invalid_argument("truncated replication frame");
    std::uint64_t n=0; for (unsigned i=0; i<count; ++i) n=(n<<8)|static_cast<unsigned char>(in[i]);
    in.remove_prefix(count); return n;
}
std::string entity_key(const std::string& type, const std::string& id) {
    if (type.empty() || type.size()>127 || type.find_first_of(".\0",0,2)!=std::string::npos || id.empty() || id.size()>256)
        throw std::invalid_argument("invalid replication entity");
    std::string out; put(out,type.size(),1); out+=type; put(out,id.size(),2); out+=id; return out;
}
std::pair<std::string,std::string> read_key(std::string_view& in) {
    auto n=get(in,1); if(n>in.size()) throw std::invalid_argument("truncated entity type");
    std::string type(in.substr(0,n)); in.remove_prefix(n);
    n=get(in,2); if(n>in.size()) throw std::invalid_argument("truncated entity id");
    std::string id(in.substr(0,n)); in.remove_prefix(n); (void)entity_key(type,id); return {type,id};
}
std::string token(std::uint64_t id) { std::string out; put(out,id,8); return out; }
struct Packet { std::uint64_t subscription; std::string type,id; Status status; std::uint32_t lease_ms; std::optional<AttributeUpdate> update; };
std::string packet(std::uint64_t subscription, const std::string& type, const std::string& id, Status status, const AttributeUpdate* update=nullptr, std::uint32_t lease_ms=0) {
    std::string out="FSN1"; put(out,static_cast<unsigned>(status),1); put(out,subscription,8); put(out,lease_ms,4); out+=entity_key(type,id);
    if(status==Status::ok) { if(!update) throw std::invalid_argument("missing property update"); out+=encode_attributes(*update); }
    return out;
}
Packet parse(std::string_view in) {
    if(in.size()>1024*1024 || !in.starts_with("FSN1")) throw std::invalid_argument("invalid replication packet");
    in.remove_prefix(4); auto status=get(in,1); if(status>static_cast<unsigned>(Status::internal)) throw std::invalid_argument("invalid replication status");
    auto subscription=get(in,8); if(!subscription) throw std::invalid_argument("invalid subscription");
    auto lease_ms=static_cast<std::uint32_t>(get(in,4));
    auto [type,id]=read_key(in); Packet result{subscription,std::move(type),std::move(id),static_cast<Status>(status),lease_ms,{}};
    if(result.status==Status::ok) result.update=decode_attributes(in);
    else if(!in.empty()) throw std::invalid_argument("trailing revocation bytes");
    return result;
}
}
struct AttributeReplication::Impl : std::enable_shared_from_this<Impl> {
    struct Subscription {
        std::uint64_t id; CallIdentity identity; std::string type,entity;
        Clock::time_point expires; Reply pending;
        AttributeUpdate last; std::size_t bytes=0;
        std::string permission; std::uint64_t generation=0;
    };
    Node& node; std::shared_ptr<EntityScriptHost> host; Observer observer; ReplicationLimits limits;
    std::unordered_map<std::uint64_t,std::shared_ptr<Subscription>> subscriptions;
    std::unordered_map<std::string,std::unordered_set<std::uint64_t>> by_entity;
    std::unordered_map<std::uint64_t,std::size_t> peer_counts;
    std::deque<std::uint64_t> schedule; std::uint64_t serial=0; std::size_t bytes=0; bool stopped=false;
    std::vector<std::string> bound; bool listener_owned=false;
    Impl(Node& n,std::shared_ptr<EntityScriptHost> h,Observer o,ReplicationLimits l) : node(n),host(std::move(h)),observer(std::move(o)),limits(l) {
        if(!host || !limits.subscriptions || !limits.per_peer || !limits.image_bytes || limits.pending_timeout.count()<=0 || limits.lease<std::chrono::seconds(3) || limits.lease.count()>UINT32_MAX)
            throw std::invalid_argument("invalid replication limits");
    }
    struct Permission { std::string owner,key; bool observer; };
    Permission permission(const Subscription& sub) {
        auto authenticated=node.identity(sub.identity.peer);
        if(!authenticated || authenticated->caller!=Caller::client || authenticated->subject!=sub.identity.subject)
            throw std::runtime_error("connection identity changed");
        auto owner=host->owner_subject(sub.type,sub.entity);
        bool observes=false; if(observer) { auto check=observer; observes=check(sub.identity,sub.type,sub.entity); }
        if(stopped || (sub.identity.subject!=owner && !observes) || sub.identity.subject.empty()) throw std::runtime_error("subscription denied");
        return {owner,std::to_string(owner.size())+":"+owner+(observes?"1":"0"),observes};
    }
    AttributeUpdate filtered(const Subscription& sub, const EntitySchema& schema, const AttributeUpdate& image, std::string* key=nullptr) {
        auto view=permission(sub); if(key) *key=view.key;
        EntityAttributes model(schema,view.owner); model.restore_full(image.values,image.revision);
        return model.snapshot({sub.identity.subject,view.observer});
    }
    void remove(std::uint64_t id, Status status, bool notify=true) {
        auto it=subscriptions.find(id); if(it==subscriptions.end()) return;
        auto sub=it->second; subscriptions.erase(it); std::erase(schedule,id); bytes-=sub->bytes;
        auto indexed=by_entity.find(entity_key(sub->type,sub->entity));
        if(indexed!=by_entity.end()) { indexed->second.erase(id); if(indexed->second.empty()) by_entity.erase(indexed); }
        auto peers=peer_counts.find(sub->identity.peer); if(peers!=peer_counts.end() && --peers->second==0) peer_counts.erase(peers);
        if(sub->pending) { auto reply=std::exchange(sub->pending,{}); reply({status,{}}); }
        else if(notify && node.connected(sub->identity.peer)) {
            if(!node.notify(sub->identity.peer,channel,packet(id,sub->type,sub->entity,status))) node.close(sub->identity.peer);
        }
    }
    void watch(const CallIdentity& identity, std::string input, Reply reply) {
        std::string type,id;
        try { std::string_view view(input); auto key=read_key(view); if(!view.empty()) throw std::invalid_argument("trailing watch bytes"); type=std::move(key.first); id=std::move(key.second); }
        catch(...) { return reply({Status::rejected,{}}); }
        if(stopped || identity.subject.empty()) return reply({Status::rejected,{}});
        std::size_t peers=peer_counts.contains(identity.peer)?peer_counts.at(identity.peer):0;
        if(subscriptions.size()>=limits.subscriptions || peers>=limits.per_peer || serial==UINT64_MAX) return reply({Status::overloaded,{}});
        auto definition=host->schema(type); if(!definition) return reply({Status::not_found,{}});
        // Check permission before creating or loading an entity. The completion
        // rechecks it and filters against the then-current ownership/AOI mapping.
        try { (void)permission(Subscription{0,identity,type,id,{}, {}, {},0,{},0}); }
        catch(...) { return reply({Status::rejected,{}}); }
        auto sub=std::make_shared<Subscription>(Subscription{++serial,identity,std::move(type),std::move(id),Clock::now()+limits.pending_timeout,std::move(reply),{},0,{},0});
        subscriptions.emplace(sub->id,sub); by_entity[entity_key(sub->type,sub->entity)].insert(sub->id); ++peer_counts[identity.peer]; schedule.push_back(sub->id);
        try {
            host->read_attributes(sub->type,sub->entity,[weak=weak_from_this(),id=sub->id,definition](Result result) {
                auto self=weak.lock(); if(!self || self->stopped) return;
                auto it=self->subscriptions.find(id); if(it==self->subscriptions.end()) return;
                auto sub=it->second;
                if(result.status!=Status::ok) return self->remove(id,result.status,false);
                try {
                    std::string permission;
                    auto next=self->filtered(*sub,*definition,decode_attributes(result.data),&permission);
                    if(self->stopped || !self->subscriptions.contains(id)) return;
                    auto count=encode_attributes(next).size();
                    if(count>512*1024) return self->remove(id,Status::overloaded,false);
                    if(count>self->limits.image_bytes-self->bytes) return self->remove(id,Status::overloaded,false);
                    auto response=packet(id,sub->type,sub->entity,Status::ok,&next,static_cast<std::uint32_t>(self->limits.lease.count()));
                    auto handle=self->host->handle(sub->type,sub->entity); if(!handle) return self->remove(id,Status::rejected,false);
                    sub->generation=handle->generation; sub->permission=std::move(permission);
                    self->bytes+=count; sub->bytes=count; sub->last=std::move(next); sub->expires=Clock::now()+self->limits.lease;
                    auto reply=std::exchange(sub->pending,{}); reply({Status::ok,std::move(response)});
                } catch(...) { self->remove(id,Status::rejected,false); }
            });
        } catch(...) { remove(sub->id,Status::internal,false); }
    }
    void control(const CallIdentity& identity,std::string input,Reply reply,bool renew) {
        std::uint64_t id;
        try { std::string_view in(input); id=get(in,8); if(!in.empty()) throw std::invalid_argument("invalid control bytes"); }
        catch(...) { return reply({Status::rejected,{}}); }
        auto it=subscriptions.find(id); if(it==subscriptions.end()) return reply({Status::not_found,{}});
        auto sub=it->second;
        if(sub->identity.peer!=identity.peer || sub->identity.subject!=identity.subject) return reply({Status::rejected,{}});
        if(renew) {
            if(sub->pending || sub->expires<=Clock::now()) return reply({Status::timeout,{}});
            sub->expires=Clock::now()+limits.lease;
        } else remove(id,Status::disconnected,false);
        reply({Status::ok,{}});
    }
    void committed(const EntityScriptHost::AttributeCommit& event) {
        std::vector<std::uint64_t> ids;
        auto interested=by_entity.find(entity_key(event.type,event.id));
        if(interested==by_entity.end()) return;
        ids.assign(interested->second.begin(),interested->second.end());
        for(auto id:ids) {
            if(stopped) break;
            auto it=subscriptions.find(id); if(it==subscriptions.end()) continue;
            auto sub=it->second; if(sub->pending) continue;
            if(sub->expires<=Clock::now()) { remove(id,Status::timeout); continue; }
            try {
                auto handle=host->handle(sub->type,sub->entity);
                if(!handle || handle->generation!=sub->generation) { remove(id,Status::disconnected); continue; }
                std::string permission;
                auto next=filtered(*sub,*event.schema,event.current,&permission);
                if(stopped || !subscriptions.contains(id)) continue;
                AttributeUpdate update{sub->last.revision,next.revision,false,{}};
                bool replace=next.revision<sub->last.revision;
                for(const auto& [name,value]:sub->last.values) { (void)value; if(!next.values.contains(name)) replace=true; }
                for(const auto& [name,value]:next.values) if(!sub->last.values.contains(name) || sub->last.values.at(name)!=value) update.values.emplace(name,value);
                if(replace) update=next;
                else if(update.values.empty()) continue;
                auto count=encode_attributes(next).size();
                if(count>limits.image_bytes-bytes+sub->bytes) { remove(id,Status::overloaded); continue; }
                if(!node.notify(sub->identity.peer,channel,packet(id,sub->type,sub->entity,Status::ok,&update))) { node.close(sub->identity.peer); remove(id,Status::disconnected,false); continue; }
                bytes=bytes-sub->bytes+count; sub->bytes=count; sub->last=std::move(next); sub->permission=std::move(permission);
            } catch(...) { remove(id,Status::rejected); }
        }
    }
    void stop() {
        if(std::exchange(stopped,true)) return;
        if(listener_owned) host->on_attributes({});
        for(const auto& route:bound) node.unbind(route);
        bound.clear();
        auto previous=std::move(subscriptions); subscriptions.clear(); by_entity.clear(); peer_counts.clear(); schedule.clear(); bytes=0;
        for(auto& [id,sub]:previous) {
            if(sub->pending) { try { sub->pending({Status::disconnected,{}}); } catch(...) {} }
            else { try { node.notify(sub->identity.peer,channel,packet(id,sub->type,sub->entity,Status::disconnected)); } catch(...) {} }
        }
    }
};
AttributeReplication::AttributeReplication(Node& node,std::shared_ptr<EntityScriptHost> host,Observer observer,ReplicationLimits limits)
 : impl_(std::make_shared<Impl>(node,std::move(host),std::move(observer),limits)) {
    try {
        auto bind=[&](std::string route,ContextHandler h) { impl_->bound.push_back(route); node.bind_rpc(std::move(route),{true,false,false},std::move(h)); };
        // Reserve names first so failure cannot unbind a pre-existing application route.
        for(auto route:{"system.attributes.watch","system.attributes.unwatch","system.attributes.renew"})
            if(node.has_route(route)) throw std::logic_error("replication route already registered");
        bind("system.attributes.watch",[weak=std::weak_ptr(impl_)](const CallIdentity& who,std::string data,Reply reply) { if(auto self=weak.lock()) self->watch(who,std::move(data),std::move(reply)); else reply({Status::disconnected,{}}); });
        bind("system.attributes.unwatch",[weak=std::weak_ptr(impl_)](const CallIdentity& who,std::string data,Reply reply) { if(auto self=weak.lock()) self->control(who,std::move(data),std::move(reply),false); else reply({Status::disconnected,{}}); });
        bind("system.attributes.renew",[weak=std::weak_ptr(impl_)](const CallIdentity& who,std::string data,Reply reply) { if(auto self=weak.lock()) self->control(who,std::move(data),std::move(reply),true); else reply({Status::disconnected,{}}); });
        impl_->host->on_attributes([weak=std::weak_ptr(impl_)](const auto& event) { if(auto self=weak.lock()) self->committed(event); });
        impl_->listener_owned=true;
    } catch(...) { impl_->stop(); throw; }
}
AttributeReplication::~AttributeReplication() { stop(); }
void AttributeReplication::stop() { impl_->stop(); }
std::size_t AttributeReplication::poll(std::size_t budget) {
    auto self=impl_; std::size_t work=0, count=self->schedule.size();
    while(!self->stopped && work<budget && count-- && !self->schedule.empty()) {
        auto id=self->schedule.front(); self->schedule.pop_front();
        auto it=self->subscriptions.find(id); if(it==self->subscriptions.end()) continue;
        auto sub=it->second;
        if(!self->node.connected(sub->identity.peer)) self->remove(id,Status::disconnected,false);
        else if(sub->expires<=Clock::now()) self->remove(id,Status::timeout);
        else {
            if(!sub->pending) try { auto definition=self->host->schema(sub->type); if(!definition) throw std::runtime_error("schema removed");
                    auto handle=self->host->handle(sub->type,sub->entity);
                    if(!handle || handle->generation!=sub->generation) throw std::runtime_error("entity deactivated");
                    auto permission=self->permission(*sub);
                    if(permission.key!=sub->permission) throw std::runtime_error("visibility changed: refresh snapshot"); }
                catch(...) { self->remove(id,Status::rejected); }
            if(self->subscriptions.contains(id)) self->schedule.push_back(id);
        }
        ++work;
    }
    return work;
}
struct AttributeClient::Impl : std::enable_shared_from_this<Impl> {
    struct Projection { std::string type,entity; AttributeReplica replica; Clock::time_point renewed; std::chrono::milliseconds interval{10000}; std::size_t bytes=0; };
    struct Pending { std::string type,entity; Ready ready; };
    Node& node; std::uint64_t peer; Changed changed; bool stopped=false;
    std::uint64_t serial=0; std::size_t bytes=0;
    std::unordered_map<std::uint64_t,std::shared_ptr<Projection>> projections;
    std::unordered_map<std::uint64_t,Pending> pending;
    Impl(Node& n,std::uint64_t p,Changed c):node(n),peer(p),changed(std::move(c)) { if(!peer) throw std::invalid_argument("server connection required"); }
    static void ready_callback(Ready& callback, Status status, std::uint64_t id) {
        try { callback(status,id); } catch(...) {}
    }
    void change(std::uint64_t id,Status status,const AttributeReplica* replica) {
        try { if(changed) { auto callback=changed; callback(id,status,replica); } } catch(...) {}
    }
    void remote_unwatch(std::uint64_t id) {
        node.call(peer,"system.attributes.unwatch",token(id),std::chrono::seconds(1),[](Result) {});
    }
    void remove(std::uint64_t id,Status status,bool remote) {
        auto it=projections.find(id); if(it==projections.end()) return;
        auto projection=it->second; projections.erase(it); bytes-=projection->bytes;
        if(remote) remote_unwatch(id);
        change(id,status,nullptr);
    }
    void receive(std::uint64_t from,std::string data) {
        if(stopped || from!=peer) return; // an unrelated authenticated peer cannot inject updates
        try {
            auto message=parse(data); auto it=projections.find(message.subscription); if(it==projections.end()) return;
            auto projection=it->second;
            if(message.type!=projection->type || message.id!=projection->entity) throw std::runtime_error("subscription identity mismatch");
            if(message.status!=Status::ok) return remove(message.subscription,message.status,false);
            auto next=projection->replica;
            if(!next.apply(*message.update)) return remove(message.subscription,Status::rejected,true);
            auto count=encode_attributes({0,next.revision(),true,next.values()}).size();
            if(count>16*1024*1024-bytes+projection->bytes) return remove(message.subscription,Status::overloaded,true);
            bytes=bytes-projection->bytes+count; projection->bytes=count; projection->replica=std::move(next);
            change(message.subscription,Status::ok,&projection->replica);
        } catch(...) { node.close(peer); stop(); }
    }
    void watch(std::string type,std::string id,Ready ready,std::chrono::milliseconds timeout) {
        if(!ready) throw std::invalid_argument("subscription callback required");
        if(stopped) return ready_callback(ready,Status::disconnected,0);
        if(pending.size()+projections.size()>=256 || serial==UINT64_MAX) return ready_callback(ready,Status::overloaded,0);
        std::string input;
        try { input=entity_key(type,id); } catch(...) { return ready_callback(ready,Status::rejected,0); }
        auto request=++serial; pending.emplace(request,Pending{std::move(type),std::move(id),std::move(ready)});
        node.call(peer,"system.attributes.watch",std::move(input),timeout,[self=shared_from_this(),request](Result result) {
            auto it=self->pending.find(request);
            if(it==self->pending.end()) {
                if(result.status==Status::ok) try { self->remote_unwatch(parse(result.data).subscription); } catch(...) {}
                return;
            }
            auto pending=std::move(it->second); self->pending.erase(it);
            if(result.status!=Status::ok) return ready_callback(pending.ready,result.status,0);
            std::uint64_t subscription=0;
            try {
                auto message=parse(result.data); subscription=message.subscription;
                if(message.status!=Status::ok || message.lease_ms<3000 || !message.update->snapshot || message.type!=pending.type || message.id!=pending.entity || self->projections.contains(subscription))
                    throw std::runtime_error("invalid watch response");
                auto projection=std::make_shared<Projection>(Projection{std::move(pending.type),std::move(pending.entity),{},Clock::now(),std::chrono::milliseconds(message.lease_ms/3),0});
                if(!projection->replica.apply(*message.update)) throw std::runtime_error("invalid initial snapshot");
                auto count=encode_attributes({0,projection->replica.revision(),true,projection->replica.values()}).size();
                if(count>16*1024*1024-self->bytes) throw std::length_error("client replica budget exceeded");
                projection->bytes=count;
                self->projections.emplace(subscription,projection); self->bytes+=count;
                ready_callback(pending.ready,Status::ok,subscription);
                if(!self->stopped && self->projections.contains(subscription)) self->change(subscription,Status::ok,&projection->replica);
            } catch(...) {
                if(subscription) self->remote_unwatch(subscription);
                if(subscription && self->projections.contains(subscription)) self->remove(subscription,Status::internal,false);
                else ready_callback(pending.ready,Status::rejected,0);
            }
        });
    }
    void stop() {
        if(std::exchange(stopped,true)) return;
        node.off_notification(channel);
        auto requests=std::move(pending); pending.clear();
        auto previous=std::move(projections); projections.clear(); bytes=0;
        for(auto& [id,sub]:previous) { remote_unwatch(id); change(id,Status::disconnected,nullptr); }
        for(auto& [id,request]:requests) { (void)id; try { request.ready(Status::disconnected,0); } catch(...) {} }
    }
};
AttributeClient::AttributeClient(Node& node,std::uint64_t peer,Changed changed)
 : impl_(std::make_shared<Impl>(node,peer,std::move(changed))) {
    node.on_notification(channel,[weak=std::weak_ptr(impl_)](std::uint64_t from,const std::string&,std::string data) {
        if(auto self=weak.lock()) self->receive(from,std::move(data));
    });
}
AttributeClient::~AttributeClient() { stop(); }
void AttributeClient::watch(std::string type,std::string id,Ready ready,std::chrono::milliseconds timeout) { impl_->watch(std::move(type),std::move(id),std::move(ready),timeout); }
void AttributeClient::unwatch(std::uint64_t id) { auto self=impl_; self->remove(id,Status::disconnected,true); }
const AttributeReplica* AttributeClient::get(std::uint64_t id) const { auto it=impl_->projections.find(id); return it==impl_->projections.end()?nullptr:&it->second->replica; }
void AttributeClient::stop() { impl_->stop(); }
void AttributeClient::poll() {
    auto self=impl_; if(self->stopped) return;
    if(!self->node.connected(self->peer)) {
        // Connections may still be completing their initial handshake while watches are pending.
        if(!self->projections.empty()) self->stop();
        return;
    }
    std::vector<std::uint64_t> renew;
    for(const auto& [id,projection]:self->projections) if(Clock::now()-projection->renewed>=projection->interval) renew.push_back(id);
    for(auto id:renew) {
        auto it=self->projections.find(id); if(it==self->projections.end()) continue;
        it->second->renewed=Clock::now();
        self->node.call(self->peer,"system.attributes.renew",token(id),std::chrono::seconds(1),[weak=std::weak_ptr(self),id](Result result) {
            if(auto client=weak.lock(); client && !client->stopped && result.status!=Status::ok) client->remove(id,result.status,false);
        });
    }
}
}

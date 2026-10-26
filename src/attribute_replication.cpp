#include "fsnet/attribute_replication.hpp"
#include "fsnet/replication_wire.hpp"
#include <algorithm>
#include <deque>
#include <limits>
#include <random>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace fsnet {
namespace {
using Clock = std::chrono::steady_clock;
constexpr const char* channel = "system.attributes.update";
constexpr const char* presence_channel = "system.entities.presence";
void put(std::string& out, std::uint64_t value, unsigned count) {
    for (unsigned i=count; i; --i) out += static_cast<char>(value >> ((i-1)*8));
}
std::uint64_t read_uint(std::string_view& in, unsigned count) {
    if (in.size() < count) throw std::invalid_argument("truncated replication frame");
    std::uint64_t n=0; for (unsigned i=0; i<count; ++i) n=(n<<8)|static_cast<unsigned char>(in[i]);
    in.remove_prefix(count); return n;
}
std::string token(std::uint64_t id) { std::string out; put(out,id,8); return out; }

}
struct AttributeReplication::Impl : std::enable_shared_from_this<Impl> {
    struct Subscription {
        std::uint64_t id; CallIdentity identity; std::string type,entity;
        Clock::time_point expires; Reply pending;
        AttributeUpdate last; std::size_t bytes=0;
        std::string permission; std::uint64_t generation=0;
        bool automatic=false,controlled=false,owned=false;
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
    Permission permission(const Subscription& sub,const std::string* metadata_owner=nullptr) {
        auto authenticated=node.identity(sub.identity.peer);
        if(!authenticated || authenticated->caller!=Caller::client || authenticated->subject!=sub.identity.subject || authenticated->connection!=sub.identity.connection || !sub.identity.live() || stopped)
            throw std::runtime_error("connection identity changed");
        auto owner=metadata_owner?*metadata_owner:host->owner_binding(sub.type,sub.entity);
        bool observes=sub.automatic; if(observer) { auto check=observer; observes=check(sub.identity,sub.type,sub.entity) || observes; }
        if(stopped)throw std::runtime_error("subscription denied");
        auto owned=sub.identity.owns(owner);
        auto current=subscriptions.find(sub.id);
        if(stopped || current==subscriptions.end() || current->second.get()!=&sub || (!owned && !observes) || (!sub.identity.connection && sub.identity.subject.empty())) throw std::runtime_error("subscription denied");
        return {owner,std::to_string(owner.size())+":"+owner+(observes?"1":"0"),observes};
    }
    AttributeUpdate filtered(const Subscription& sub, const EntitySchema& schema, const AttributeUpdate& image, std::string* key=nullptr) {
        auto view=permission(sub); if(key) *key=view.key;
        // Host commit images have already passed schema validation. Project
        // directly instead of rebuilding defaults and restoring a carrier for
        // every recipient. Authorization is still rechecked for each peer.
        AttributeUpdate result{0,image.revision,true,{}};
        const bool owned=sub.identity.owns(view.owner);
        for (const auto& [name,d]:schema.attributes) {
            const bool readable=d.client==Access::read || d.client==Access::read_write;
            if (readable && ((d.visibility==Visibility::owner && owned) ||
                (d.visibility==Visibility::observers && (owned || view.observer))))
                result.values.emplace(name,image.values.at(name));
        }
        return result;
    }
    std::string presence(const Subscription& sub,EntityPresence::Kind kind,Status status=Status::ok) {
        auto owned=sub.owned;
        std::string out; out+=static_cast<char>(kind); out+=static_cast<char>((owned?1:0)|(sub.controlled?2:0));
        out+=encode_replication_packet(sub.id,sub.type,sub.entity,status,status==Status::ok?&sub.last:nullptr,static_cast<std::uint32_t>(limits.lease.count()));
        return out;
    }
    void remove(std::uint64_t id, Status status, bool notify=true) {
        auto it=subscriptions.find(id); if(it==subscriptions.end()) return;
        auto sub=it->second; subscriptions.erase(it); std::erase(schedule,id); bytes-=sub->bytes;
        auto indexed=by_entity.find(encode_entity_key(sub->type,sub->entity));
        if(indexed!=by_entity.end()) { indexed->second.erase(id); if(indexed->second.empty()) by_entity.erase(indexed); }
        auto peers=peer_counts.find(sub->identity.peer); if(peers!=peer_counts.end() && --peers->second==0) peer_counts.erase(peers);
        if(sub->pending) { auto reply=std::exchange(sub->pending,{}); reply({status,{}}); }
        else if(notify && node.connected(sub->identity.peer)) {
            auto data=sub->automatic?presence(*sub,EntityPresence::Kind::leave,status):encode_replication_packet(id,sub->type,sub->entity,status);
            if(!node.notify(sub->identity.peer,sub->automatic?presence_channel:channel,std::move(data))) node.close(sub->identity.peer);
        }
    }
    void watch(const CallIdentity& identity, std::string input, Reply reply,bool automatic=false) {
        std::string type,id;
        try { std::string_view view(input); auto key=decode_entity_key(view); if(!view.empty()) throw std::invalid_argument("trailing watch bytes"); type=std::move(key.first); id=std::move(key.second); }
        catch(...) { return reply({Status::rejected,{}}); }
        if(stopped || (!identity.connection && identity.subject.empty())) return reply({Status::rejected,{}});
        std::size_t peers=peer_counts.contains(identity.peer)?peer_counts.at(identity.peer):0;
        if(subscriptions.size()>=limits.subscriptions || peers>=limits.per_peer || serial==UINT64_MAX) return reply({Status::overloaded,{}});
        auto definition=host->schema(type); if(!definition) return reply({Status::not_found,{}});
        if(!valid_entity_id(id)) return reply({Status::rejected,{}});
        auto sub=std::make_shared<Subscription>(Subscription{++serial,identity,std::move(type),std::move(id),Clock::now()+limits.pending_timeout,std::move(reply),{},0,{},0,automatic,false});
        subscriptions.emplace(sub->id,sub); by_entity[encode_entity_key(sub->type,sub->entity)].insert(sub->id); ++peer_counts[identity.peer]; schedule.push_back(sub->id);
        try {
            // Authorization uses persisted metadata before acquiring a lease or
            // executing lifecycle hooks. The host rechecks the guard against
            // the acquired record and before publication as ownership/AOI can
            // change while storage and script work are outstanding.
            host->read_attributes(sub->type,sub->entity,
                [weak=weak_from_this(),id=sub->id](const std::string& owner) {
                    auto self=weak.lock(); if(!self || self->stopped) return false;
                    auto it=self->subscriptions.find(id);
                    if(it==self->subscriptions.end() || it->second->expires<=Clock::now()) return false;
                    auto sub=it->second; // resolver/AOI callbacks can stop and erase the map
                    try { (void)self->permission(*sub,&owner); return true; }
                    catch(...) { return false; }
                },[weak=weak_from_this(),id=sub->id,definition](Result result) {
                auto self=weak.lock(); if(!self || self->stopped) return;
                auto it=self->subscriptions.find(id); if(it==self->subscriptions.end()) return;
                auto sub=it->second;
                if(result.status!=Status::ok) return self->remove(id,result.status,false);
                try {
                    std::string permission;
                    auto next=self->filtered(*sub,*definition,decode_attributes(result.data),&permission);
                    if(self->stopped || !self->subscriptions.contains(id)) return;
                    auto count=attribute_wire_size(next)+attribute_image_bytes(next.values);
                    if(count>512*1024) return self->remove(id,Status::overloaded,false);
                    if(count>self->limits.image_bytes-self->bytes) return self->remove(id,Status::overloaded,false);
                    auto response=encode_replication_packet(id,sub->type,sub->entity,Status::ok,&next,static_cast<std::uint32_t>(self->limits.lease.count()));
                    auto handle=self->host->handle(sub->type,sub->entity); if(!handle) return self->remove(id,Status::rejected,false);
                    sub->owned=sub->identity.owns(self->host->owner_binding(sub->type,sub->entity));
                    sub->generation=handle->generation; sub->permission=std::move(permission);
                    self->bytes+=count; sub->bytes=count; sub->last=std::move(next); sub->expires=Clock::now()+self->limits.lease;
                    auto reply=std::exchange(sub->pending,{}); reply({Status::ok,std::move(response)});
                } catch(...) { self->remove(id,Status::rejected,false); }
            });
        } catch(...) { remove(sub->id,Status::internal,false); }
    }
    void visibility(std::uint64_t peer,std::string type,std::string entity,unsigned action,Reply reply) {
        if(stopped) return reply({Status::disconnected,{}});
        auto who=node.identity(peer);
        if(!who || who->caller!=Caller::client || (!who->connection && who->subject.empty())) return reply({Status::rejected,{}});
        std::string input;
        try { input=encode_entity_key(type,entity); } catch(...) { return reply({Status::rejected,{}}); }
        std::shared_ptr<Subscription> target;
        auto indexed=by_entity.find(input);
        if(indexed!=by_entity.end()) for(auto id:indexed->second) {
            auto sub=subscriptions.at(id);
            if(sub->automatic && sub->identity.peer==peer && sub->identity.subject==who->subject && sub->identity.connection==who->connection) { target=sub; break; }
        }
        if(target && !target->pending) try {
            auto handle=host->handle(type,entity);
            if(target->expires<=Clock::now() || !handle || handle->generation!=target->generation ||
               !host->schema(type) || permission(*target).key!=target->permission)
                throw std::runtime_error("stale entity visibility");
        } catch(...) { remove(target->id,Status::rejected); target.reset(); }
        if(stopped) return reply({Status::disconnected,{}});
        if(action==1) {
            if(target) return reply({target->pending?Status::overloaded:Status::ok,
                target->pending?std::string{}:presence(*target,EntityPresence::Kind::enter)});
            // show only publishes an already committed/resident entity. It must
            // not create default entities merely by granting scene visibility.
            if(!host->active(type,entity)) return reply({Status::not_found,{}});
            return watch(*who,std::move(input),[weak=weak_from_this(),peer,reply=std::move(reply)](Result result) mutable {
                auto self=weak.lock(); if(!self || self->stopped) return reply({Status::disconnected,{}});
                if(result.status!=Status::ok) return reply(std::move(result));
                auto message=decode_replication_packet(result.data); auto it=self->subscriptions.find(message.subscription);
                if(it==self->subscriptions.end()) return reply({Status::disconnected,{}});
                if(!self->node.notify(peer,presence_channel,self->presence(*it->second,EntityPresence::Kind::enter))) {
                    self->node.close(peer); self->remove(message.subscription,Status::disconnected,false);
                    return reply({Status::disconnected,{}});
                }
                reply({Status::ok,self->presence(*it->second,EntityPresence::Kind::enter)});
            },true);
        }
        if(action==2) { if(target) remove(target->id,Status::disconnected); return reply({Status::ok,{}}); }
        if(action!=3 && action!=4) return reply({Status::rejected,{}});
        if(!target || target->pending) return reply({Status::not_found,{}});
        try { (void)permission(*target); }
        catch(...) { return reply({Status::rejected,{}}); }
        if(action==3 && !who->owns(host->owner_binding(type,entity))) return reply({Status::rejected,{}});
        auto set_control=[&](const std::shared_ptr<Subscription>& sub,bool value) {
            if(sub->controlled==value) return true;
            sub->controlled=value;
            if(node.notify(peer,presence_channel,presence(*sub,EntityPresence::Kind::control))) return true;
            node.close(peer); return false;
        };
        if(action==3) for(const auto& [id,sub]:subscriptions) {
            (void)id;
            if(sub!=target && sub->automatic && sub->identity.peer==peer && sub->controlled)
                if(!set_control(sub,false)) return reply({Status::disconnected,{}});
        }
        if(!set_control(target,action==3)) return reply({Status::disconnected,{}});
        reply({Status::ok,presence(*target,EntityPresence::Kind::enter)});
    }
    void control(const CallIdentity& identity,std::string input,Reply reply,bool renew) {
        std::uint64_t id;
        try { std::string_view in(input); id=read_uint(in,8); if(!in.empty()) throw std::invalid_argument("invalid control bytes"); }
        catch(...) { return reply({Status::rejected,{}}); }
        auto it=subscriptions.find(id); if(it==subscriptions.end()) return reply({Status::not_found,{}});
        auto sub=it->second;
        if(sub->identity.peer!=identity.peer || sub->identity.subject!=identity.subject || sub->identity.connection!=identity.connection || !identity.live()) return reply({Status::rejected,{}});
        if(renew) {
            if(sub->pending || sub->expires<=Clock::now()) return reply({Status::timeout,{}});
            try { (void)permission(*sub); } catch(...) { return reply({Status::rejected,{}}); }
            sub->expires=Clock::now()+limits.lease;
        } else remove(id,Status::disconnected,false);
        reply({Status::ok,{}});
    }
    void write(const CallIdentity& who, std::string input, Reply reply) {
        std::uint64_t id; std::string request, proposal;
        try {
            std::string_view in(input); id=read_uint(in,8); auto length=read_uint(in,2);
            if(!length || length>128 || length>in.size()) throw std::invalid_argument("invalid write request ID");
            request=std::string(in.substr(0,length)); in.remove_prefix(length);
            auto update=decode_attributes(in);
            if(update.snapshot || update.base!=update.revision) throw std::invalid_argument("invalid write proposal");
            proposal=std::string(in);
        } catch(...) { return reply({Status::rejected,{}}); }
        auto it=subscriptions.find(id); if(it==subscriptions.end()) return reply({Status::not_found,{}});
        auto sub=it->second;
        if(sub->pending) return reply({Status::overloaded,{}});
        if(sub->identity.peer!=who.peer || sub->identity.subject!=who.subject || sub->identity.connection!=who.connection) return reply({Status::rejected,{}});
        if(sub->expires<=Clock::now()) return reply({Status::timeout,{}});
        try { auto view=permission(*sub); if(!who.owns(view.owner)) return reply({Status::rejected,{}}); }
        catch(...) { return reply({Status::rejected,{}}); }
        if(stopped || !subscriptions.contains(id)) return reply({Status::disconnected,{}});
        host->write_attributes(sub->type,sub->entity,who,std::move(proposal),std::move(reply),std::move(request));
    }
    void committed(const EntityScriptHost::AttributeCommit& event) {
        std::vector<std::uint64_t> ids;
        auto interested=by_entity.find(encode_entity_key(event.type,event.id));
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
                if(permission!=sub->permission) { remove(id,Status::rejected); continue; }
                auto update=attribute_delta(sub->last,next);
                if(!update.snapshot && update.values.empty() && update.patches.empty() &&
                   (next.revision==sub->last.revision || !sub->identity.owns(host->owner_binding(sub->type,sub->entity)))) continue;
                // Owners need even an empty delta to advance their optimistic-write
                // revision after server-private changes. Observers may skip it.
                auto count=attribute_wire_size(next)+attribute_image_bytes(next.values);
                if(count>limits.image_bytes-bytes+sub->bytes) { remove(id,Status::overloaded); continue; }
                if(!node.notify(sub->identity.peer,channel,encode_replication_packet(id,sub->type,sub->entity,Status::ok,&update))) { node.close(sub->identity.peer); remove(id,Status::disconnected,false); continue; }
                bytes=bytes-sub->bytes+count; sub->bytes=count; sub->last=std::move(next); sub->permission=std::move(permission);
            } catch(...) { remove(id,Status::rejected); }
        }
    }
    void stop() {
        if(std::exchange(stopped,true)) return;
        if(listener_owned) { host->on_attributes({}); host->on_visibility({}); host->on_closed({}); }
        for(const auto& route:bound) node.unbind(route);
        bound.clear();
        auto previous=std::move(subscriptions); subscriptions.clear(); by_entity.clear(); peer_counts.clear(); schedule.clear(); bytes=0;
        for(auto& [id,sub]:previous) {
            if(sub->pending) { try { sub->pending({Status::disconnected,{}}); } catch(...) {} }
            else { try { node.notify(sub->identity.peer,sub->automatic?presence_channel:channel,sub->automatic?presence(*sub,EntityPresence::Kind::leave,Status::disconnected):encode_replication_packet(id,sub->type,sub->entity,Status::disconnected)); } catch(...) {} }
        }
    }
};
AttributeReplication::AttributeReplication(Node& node,std::shared_ptr<EntityScriptHost> host,Observer observer,ReplicationLimits limits)
 : impl_(std::make_shared<Impl>(node,std::move(host),std::move(observer),limits)) {
    try {
        auto bind=[&](std::string route,ContextHandler h) { impl_->bound.push_back(route); node.bind_rpc(std::move(route),{true,false,false},std::move(h)); };
        // Reserve names first so failure cannot unbind a pre-existing application route.
        for(auto route:{"system.attributes.watch","system.attributes.unwatch","system.attributes.renew","system.attributes.write"})
            if(node.has_route(route)) throw std::logic_error("replication route already registered");
        bind("system.attributes.watch",[weak=std::weak_ptr(impl_)](const CallIdentity& who,std::string data,Reply reply) { if(auto self=weak.lock()) self->watch(who,std::move(data),std::move(reply)); else reply({Status::disconnected,{}}); });
        bind("system.attributes.unwatch",[weak=std::weak_ptr(impl_)](const CallIdentity& who,std::string data,Reply reply) { if(auto self=weak.lock()) self->control(who,std::move(data),std::move(reply),false); else reply({Status::disconnected,{}}); });
        bind("system.attributes.renew",[weak=std::weak_ptr(impl_)](const CallIdentity& who,std::string data,Reply reply) { if(auto self=weak.lock()) self->control(who,std::move(data),std::move(reply),true); else reply({Status::disconnected,{}}); });
        bind("system.attributes.write",[weak=std::weak_ptr(impl_)](const CallIdentity& who,std::string data,Reply reply) { if(auto self=weak.lock()) self->write(who,std::move(data),std::move(reply)); else reply({Status::disconnected,{}}); });
        impl_->listener_owned=true;
        impl_->host->on_closed([weak=std::weak_ptr(impl_)](const std::string& type,const std::string& entity,bool destroyed) {
            if(auto self=weak.lock()) {
                auto it=self->by_entity.find(encode_entity_key(type,entity)); if(it==self->by_entity.end()) return;
                std::vector<std::uint64_t> ids(it->second.begin(),it->second.end());
                for(auto id:ids) self->remove(id,destroyed?Status::not_found:Status::disconnected);
            }
        });
        impl_->host->on_attributes([weak=std::weak_ptr(impl_)](const auto& event) { if(auto self=weak.lock()) self->committed(event); });
        impl_->host->on_visibility([weak=std::weak_ptr(impl_)](std::uint64_t peer,std::string type,std::string id,unsigned action,Reply reply) {
            if(auto self=weak.lock()) self->visibility(peer,std::move(type),std::move(id),action,std::move(reply));
            else reply({Status::disconnected,{}});
        });
        impl_->listener_owned=true;
    } catch(...) { impl_->stop(); throw; }
}
AttributeReplication::~AttributeReplication() { stop(); }
void AttributeReplication::visibility(std::uint64_t peer,std::string type,std::string id,unsigned action,Reply reply) {
    auto self=impl_; self->visibility(peer,std::move(type),std::move(id),action,std::move(reply));
}
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
    struct Projection { std::string type,entity; AttributeReplica replica; Clock::time_point renewed; std::chrono::milliseconds interval{10000}; std::size_t bytes=0; bool automatic=false,owned=false,controlled=false; };
    struct Pending { std::string type,entity; Ready ready; };
    Node& node; std::uint64_t peer; Changed changed; Presence presence_changed; bool stopped=false,entities_stopped=false;
    std::uint64_t serial=0; std::size_t bytes=0;
    std::unordered_map<std::uint64_t,std::shared_ptr<Projection>> projections;
    std::unordered_map<std::uint64_t,Pending> pending;
    std::unordered_map<std::uint64_t,Written> writes;
    std::string request_prefix;
    Impl(Node& n,std::uint64_t p,Changed c,Presence pc):node(n),peer(p),changed(std::move(c)),presence_changed(std::move(pc)) {
        if(!peer) throw std::invalid_argument("server connection required");
        std::random_device entropy; request_prefix="property-";
        for(int i=0;i<4;++i) request_prefix+=std::to_string(entropy())+'-';
    }
    static void ready_callback(Ready& callback, Status status, std::uint64_t id) {
        try { callback(status,id); } catch(...) {}
    }
    void change(std::uint64_t id,Status status,const AttributeReplica* replica) {
        try { if(changed) { auto callback=changed; callback(id,status,replica); } } catch(...) {}
    }
    void presence_event(std::uint64_t id,const Projection& p,EntityPresence::Kind kind) {
        if(p.automatic && presence_changed) try {
            auto callback=presence_changed;
            callback({kind,p.type,p.entity,id,p.owned,p.controlled});
        } catch(...) { node.close(peer); }
    }
    void receive_presence(std::uint64_t from,std::string data) {
        if(stopped || from!=peer) return;
        try {
            std::string_view in(data); auto kind=read_uint(in,1),flags=read_uint(in,1);
            if(kind<1 || kind>3 || flags>3 || ((flags&2) && !(flags&1))) throw std::invalid_argument("invalid entity presence");
            auto message=decode_replication_packet(in); auto id=message.subscription;
            auto it=projections.find(id);
            if(kind==2) {
                if(message.status==Status::ok) throw std::invalid_argument("invalid entity leave");
                if(it!=projections.end()) {
                    if(!it->second->automatic || it->second->type!=message.type || it->second->entity!=message.id)
                        throw std::invalid_argument("entity leave identity mismatch");
                    remove(id,message.status,false);
                }
                return;
            }
            if(message.status!=Status::ok || !message.update->snapshot || message.lease_ms<3000)
                throw std::invalid_argument("invalid entity creation");
            if(it==projections.end()) {
                if(entities_stopped) { remote_unwatch(id); return; }
                if(kind!=1) return; // late control after an explicitly closed object
                if(projections.size()+pending.size()>=256) throw std::length_error("entity projection limit");
                auto p=std::make_shared<Projection>(Projection{message.type,message.id,{},Clock::now(),std::chrono::milliseconds(message.lease_ms/3),0,true,bool(flags&1),bool(flags&2)});
                if(!p->replica.apply(*message.update)) throw std::invalid_argument("invalid entity snapshot");
                p->bytes=attribute_wire_size(*message.update)+attribute_image_bytes(message.update->values);
                if(p->bytes>16*1024*1024-bytes) throw std::length_error("entity image budget");
                projections.emplace(id,p); bytes+=p->bytes;
                change(id,Status::ok,&p->replica);
                if(!stopped && projections.contains(id)) presence_event(id,*p,EntityPresence::Kind::enter);
                return;
            }
            auto p=it->second;
            if(!p->automatic || p->type!=message.type || p->entity!=message.id || p->owned!=bool(flags&1))
                throw std::invalid_argument("entity presence identity mismatch");
            // Control never replaces properties: the ordinary delta stream owns
            // revisions, so duplicate/late metadata cannot rewind a projection.
            if(p->controlled!=bool(flags&2)) {
                p->controlled=bool(flags&2); presence_event(id,*p,EntityPresence::Kind::control);
            }
        } catch(...) { node.close(peer); stop(); }
    }
    void remote_unwatch(std::uint64_t id) {
        node.call(peer,"system.attributes.unwatch",token(id),std::chrono::seconds(1),[](Result) {});
    }
    void remove(std::uint64_t id,Status status,bool remote) {
        auto it=projections.find(id); if(it==projections.end()) return;
        auto projection=it->second; projections.erase(it); bytes-=projection->bytes;
        if(remote) remote_unwatch(id);
        change(id,status,nullptr); presence_event(id,*projection,EntityPresence::Kind::leave);
    }
    void receive(std::uint64_t from,std::string data) {
        if(stopped || from!=peer) return; // an unrelated authenticated peer cannot inject updates
        try {
            auto message=decode_replication_packet(data); auto it=projections.find(message.subscription); if(it==projections.end()) return;
            auto projection=it->second;
            if(message.type!=projection->type || message.id!=projection->entity) throw std::runtime_error("subscription identity mismatch");
            if(message.status!=Status::ok) return remove(message.subscription,message.status,false);
            try {
                if(!projection->replica.apply(*message.update,16*1024*1024-bytes+projection->bytes))return remove(message.subscription,Status::rejected,true);
            }catch(const AttributeReplicaBudget&){return remove(message.subscription,Status::overloaded,true);}
            auto count=projection->replica.image_bytes();
            bytes=bytes-projection->bytes+count; projection->bytes=count;
            change(message.subscription,Status::ok,&projection->replica);
        } catch(...) { node.close(peer); stop(); }
    }
    void watch(std::string type,std::string id,Ready ready,std::chrono::milliseconds timeout) {
        if(!ready) throw std::invalid_argument("subscription callback required");
        if(stopped) return ready_callback(ready,Status::disconnected,0);
        if(pending.size()+projections.size()>=256 || serial==UINT64_MAX) return ready_callback(ready,Status::overloaded,0);
        std::string input;
        try { input=encode_entity_key(type,id); } catch(...) { return ready_callback(ready,Status::rejected,0); }
        auto request=++serial; pending.emplace(request,Pending{std::move(type),std::move(id),std::move(ready)});
        node.call(peer,"system.attributes.watch",std::move(input),timeout,[self=shared_from_this(),request](Result result) {
            auto it=self->pending.find(request);
            if(it==self->pending.end()) {
                if(result.status==Status::ok) try { self->remote_unwatch(decode_replication_packet(result.data).subscription); } catch(...) {}
                return;
            }
            auto pending=std::move(it->second); self->pending.erase(it);
            if(result.status!=Status::ok) return ready_callback(pending.ready,result.status,0);
            std::uint64_t subscription=0;
            try {
                auto message=decode_replication_packet(result.data); subscription=message.subscription;
                if(message.status!=Status::ok || message.lease_ms<3000 || !message.update->snapshot || message.type!=pending.type || message.id!=pending.entity || self->projections.contains(subscription))
                    throw std::runtime_error("invalid watch response");
                auto projection=std::make_shared<Projection>(Projection{std::move(pending.type),std::move(pending.entity),{},Clock::now(),std::chrono::milliseconds(message.lease_ms/3),0});
                if(!projection->replica.apply(*message.update)) throw std::runtime_error("invalid initial snapshot");
                auto count=attribute_wire_size(projection->replica.values())+attribute_image_bytes(projection->replica.values());
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
    static void written_callback(Written& callback, Status status) { try { callback(status); } catch(...) {} }
    void write(std::uint64_t subscription, EntityAttributes::Values values, Written callback, AttributeWriteOptions options, std::chrono::milliseconds timeout) {
        if(!callback) throw std::invalid_argument("property write callback required");
        if(stopped) return written_callback(callback,Status::disconnected);
        auto it=projections.find(subscription); if(it==projections.end()) return written_callback(callback,Status::not_found);
        if(writes.size()>=256 || serial==UINT64_MAX) return written_callback(callback,Status::overloaded);
        auto request=++serial; std::string input;
        try {
            bool stable_request=!options.request_id.empty();
            if(options.request_id.empty()) options.request_id=request_prefix+std::to_string(request);
            if(options.request_id.size()>128) throw std::invalid_argument("property request ID limit");
            auto expected=options.expected_revision.value_or(it->second->replica.revision());
            input=token(subscription); put(input,options.request_id.size(),2); input+=options.request_id;
            // Explicit IDs may be retried after the confirmed baseline advances.
            // Preserve their existing byte-stable representation for deduplication.
            auto proposal=stable_request?AttributeUpdate{expected,expected,false,std::move(values)}:
                attribute_write_delta(it->second->replica.values(),it->second->replica.revision(),values,expected);
            input+=encode_attributes(proposal);
        } catch(...) { return written_callback(callback,Status::rejected); }
        writes.emplace(request,std::move(callback));
        node.call(peer,"system.attributes.write",std::move(input),timeout,[self=shared_from_this(),request](Result result) {
            auto it=self->writes.find(request); if(it==self->writes.end()) return;
            auto callback=std::move(it->second); self->writes.erase(it); written_callback(callback,result.status);
        });
    }
    void stop() {
        if(std::exchange(stopped,true)) return;
        node.off_notification(channel); node.off_notification(presence_channel);
        auto requests=std::move(pending); pending.clear();
        auto operations=std::move(writes); writes.clear();
        auto previous=std::move(projections); projections.clear(); bytes=0;
        for(auto& [id,sub]:previous) { remote_unwatch(id); change(id,Status::disconnected,nullptr); presence_event(id,*sub,EntityPresence::Kind::leave); }
        for(auto& [id,operation]:operations) { (void)id; written_callback(operation,Status::disconnected); }
        for(auto& [id,request]:requests) { (void)id; try { request.ready(Status::disconnected,0); } catch(...) {} }
    }
};
AttributeClient::AttributeClient(Node& node,std::uint64_t peer,Changed changed,Presence presence)
 : impl_(std::make_shared<Impl>(node,peer,std::move(changed),std::move(presence))) {
    node.on_notification(presence_channel,[weak=std::weak_ptr(impl_)](std::uint64_t from,const std::string&,std::string data) {
        if(auto self=weak.lock()) self->receive_presence(from,std::move(data));
    });
    node.on_notification(channel,[weak=std::weak_ptr(impl_)](std::uint64_t from,const std::string&,std::string data) {
        if(auto self=weak.lock()) self->receive(from,std::move(data));
    });
}
AttributeClient::~AttributeClient() { stop(); }
void AttributeClient::watch(std::string type,std::string id,Ready ready,std::chrono::milliseconds timeout) { impl_->watch(std::move(type),std::move(id),std::move(ready),timeout); }
void AttributeClient::write(std::uint64_t id, EntityAttributes::Values values, Written callback, AttributeWriteOptions options, std::chrono::milliseconds timeout) {
    auto self=impl_; self->write(id,std::move(values),std::move(callback),std::move(options),timeout);
}
void AttributeClient::stop_entities() {
    auto self=impl_; self->entities_stopped=true;
    std::vector<std::uint64_t> ids;
    for(const auto& [id,p]:self->projections) if(p->automatic) ids.push_back(id);
    for(auto id:ids) self->remove(id,Status::disconnected,true);
}
void AttributeClient::unwatch(std::uint64_t id) { auto self=impl_; self->remove(id,Status::disconnected,true); }
std::optional<AttributeProjectionInfo> AttributeClient::projection_info(std::uint64_t id) const {
    auto it=impl_->projections.find(id);if(it==impl_->projections.end())return {};
    const auto& p=*it->second;return AttributeProjectionInfo{id,p.type,p.entity,p.automatic,p.owned,p.controlled};
}
std::vector<AttributeProjectionInfo> AttributeClient::projection_infos() const {
    std::vector<AttributeProjectionInfo> out;out.reserve(impl_->projections.size());
    for(const auto& [id,p]:impl_->projections)out.push_back({id,p->type,p->entity,p->automatic,p->owned,p->controlled});
    return out;
}
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

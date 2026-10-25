#include "fsnet/client_gateway.hpp"
#include "fsnet/entity_message.hpp"
#include "fsnet/replication_wire.hpp"
#include <algorithm>
#include <deque>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <utility>
namespace fsnet {
namespace {
using Clock=std::chrono::steady_clock;
constexpr const char* attributes="system.attributes.update";
constexpr const char* presence="system.entities.presence";
void put(std::string& out,std::uint64_t n,unsigned count=8){for(unsigned i=count;i;--i) out+=static_cast<char>(n>>((i-1)*8));}
std::uint64_t get(std::string_view& in,unsigned count=8){if(in.size()<count) throw std::invalid_argument("truncated gateway control"); std::uint64_t n=0;for(unsigned i=0;i<count;++i)n=(n<<8)|static_cast<unsigned char>(in[i]);in.remove_prefix(count);return n;}
std::string token(std::uint64_t id){std::string out;put(out,id);return out;}
std::int64_t wall(){return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();}
}
struct GatewayDelegation::Impl:std::enable_shared_from_this<Impl>{
    struct Session{std::string subject,transport,certificate;ConnectionRef connection{};};
    Node& node;Node::IdentityResolver base;Trust trust;Restore restore;std::shared_ptr<EntityScriptHost> host;
    std::unordered_map<std::uint64_t,Session> sessions;std::size_t limit;bool stopped=false;
    Impl(Node& n,Node::IdentityResolver b,Trust t,std::shared_ptr<EntityScriptHost> h,Restore r,std::size_t l):node(n),base(std::move(b)),trust(std::move(t)),restore(std::move(r)),host(std::move(h)),limit(l){if(!base||!trust||!host||!limit)throw std::invalid_argument("gateway trust and entity host required");}
    bool valid(std::uint64_t peer,const Session& s,const std::string& cert){
        auto identity=base(peer,cert);return identity && identity->caller==Caller::server && identity->subject==s.transport && cert==s.certificate && trust(*identity,cert);
    }
    void stop(){if(std::exchange(stopped,true))return;node.unbind("system.gateway.delegate");node.unbind("system.gateway.restore");node.unbind("system.gateway.unpossess");node.unbind("system.gateway.visibility");auto copy=std::move(sessions);for(const auto& [peer,s]:copy){(void)s;node.close(peer);}node.identify(base);}
};
GatewayDelegation::GatewayDelegation(Node& node,Node::IdentityResolver base,Trust trust,std::shared_ptr<EntityScriptHost> host,Restore restore,std::size_t limit):impl_(std::make_shared<Impl>(node,std::move(base),std::move(trust),std::move(host),std::move(restore),limit)){
    if(node.has_route("system.gateway.delegate")||node.has_route("system.gateway.restore")||node.has_route("system.gateway.unpossess")||node.has_route("system.gateway.visibility"))throw std::logic_error("gateway routes already bound");
    auto weak=std::weak_ptr(impl_);
    try{
        node.bind_rpc("system.gateway.visibility",{true,false,false},[weak](const CallIdentity& who,std::string data,Reply reply){
            auto self=weak.lock();if(!self||self->stopped)return reply({Status::disconnected,{}});
            try {
                if(!self->sessions.contains(who.peer))return reply({Status::rejected,{}});
                std::string_view input(data);auto [type,id]=decode_entity_key(input);auto action=get(input,1);
                if(!input.empty() || action<1 || action>4)throw std::invalid_argument("invalid visibility request");
                self->host->read_attributes(type,id,[weak,who,type,id,action,reply=std::move(reply)](Result result)mutable{
                    auto self=weak.lock();if(!self || self->stopped)return reply({Status::disconnected,{}});
                    if(result.status!=Status::ok)return reply(std::move(result));
                    if(!who.live())return reply({Status::rejected,{}});
                    self->host->entity_visibility(who.peer,type,id,static_cast<unsigned>(action),std::move(reply));
                });
            }catch(...){if(reply)reply({Status::rejected,{}});}
        });
        node.bind_rpc("system.gateway.delegate",{false,true,false},[weak](const CallIdentity& who,std::string subject,Reply reply){
            auto self=weak.lock();if(!self||self->stopped)return reply({Status::disconnected,{}});
            auto cert=self->node.peer_identity(who.peer);
            try{
                ConnectionRef connection;
                if(!subject.starts_with("FSCN1"))throw std::invalid_argument("connection delegation required");
                {
                    std::string_view input(subject);input.remove_prefix(5);auto name=snapshot_detail::blob(input);
                    connection=ConnectionRef::from_key(snapshot_detail::blob_view(input));if(!input.empty())throw std::invalid_argument("trailing delegation");subject=std::move(name);
                }
                if((!connection)||subject.size()>256||!self->trust(who,cert)||self->sessions.contains(who.peer))return reply({Status::rejected,{}});
                if(self->sessions.size()>=self->limit)return reply({Status::overloaded,{}});
                self->sessions.emplace(who.peer,Impl::Session{std::move(subject),who.subject,std::move(cert),std::move(connection)});reply({Status::ok,{}});
            }catch(...){reply({Status::rejected,{}});}
        });
        node.bind_rpc("system.gateway.restore",{true,false,false},[weak](const CallIdentity& who,std::string data,Reply reply){
            auto self=weak.lock();if(!self||self->stopped)return reply({Status::disconnected,{}});
            try{
                if(!self->sessions.contains(who.peer))return reply({Status::rejected,{}});
                std::string_view in(data);auto [type,id]=decode_entity_key(in);auto control=get(in,1);if(!in.empty()||control>1)throw std::invalid_argument("invalid restore");
                bool allowed=who.owns(self->host->owner_binding(type,id));
                if(self->restore)allowed=self->restore(who,type,id,control!=0);
                if(!allowed && !valid_entity_id(id))return reply({Status::rejected,{}});
                self->host->read_attributes(type,id,[weak,who,type,id,control,reply=std::move(reply)](Result result)mutable{
                    auto self=weak.lock();if(!self||self->stopped)return reply({Status::disconnected,{}});
                    if(result.status!=Status::ok)return reply(std::move(result));
                    auto identity=self->node.identity(who.peer);if(!identity||identity->caller!=Caller::client||identity->subject!=who.subject)return reply({Status::rejected,{}});
                    bool allowed=who.owns(self->host->owner_binding(type,id));
                    if(self->restore)allowed=self->restore(who,type,id,control!=0);
                    if(!allowed)return reply({Status::rejected,{}});
                    self->host->entity_visibility(who.peer,type,id,1,[weak,who,type,id,control,reply=std::move(reply)](Result result)mutable{
                        auto self=weak.lock();if(!self||self->stopped)return reply({Status::disconnected,{}});
                        if(result.status!=Status::ok)return reply(std::move(result));
                        // The reply is the final snapshot AND control state,
                        // including when a previous attempt already made it visible.
                        self->host->entity_visibility(who.peer,type,id,control?3:4,std::move(reply));
                    });
                });
            }catch(...){reply({Status::rejected,{}});}
        });
        node.bind_rpc("system.gateway.unpossess",{true,false,false},[weak](const CallIdentity& who,std::string data,Reply reply){
            auto self=weak.lock();if(!self||self->stopped)return reply({Status::disconnected,{}});
            try{if(!self->sessions.contains(who.peer))return reply({Status::rejected,{}});std::string_view in(data);auto [type,id]=decode_entity_key(in);if(!in.empty())throw std::invalid_argument("control bytes");self->host->entity_visibility(who.peer,type,id,4,std::move(reply));}catch(...){reply({Status::rejected,{}});}
        });
        node.identify([weak](std::uint64_t peer,const std::string& cert)->std::optional<CallIdentity>{
            auto self=weak.lock();if(!self||self->stopped)return {};
            auto it=self->sessions.find(peer);
            if(it==self->sessions.end())return self->base(peer,cert);
            if(!self->valid(peer,it->second,cert))return {};
            return CallIdentity{Caller::client,it->second.subject,peer,it->second.connection};
        });
    }catch(...){impl_->stop();throw;}
}
GatewayDelegation::~GatewayDelegation(){stop();}
void GatewayDelegation::poll(){auto self=impl_;std::vector<std::uint64_t> dead;for(const auto& [peer,s]:self->sessions)try{if(!self->node.connected(peer)||!self->valid(peer,s,self->node.peer_identity(peer)))dead.push_back(peer);}catch(...){dead.push_back(peer);}for(auto peer:dead){self->node.close(peer);self->sessions.erase(peer);}}
void GatewayDelegation::stop(){impl_->stop();}

struct ClientGateway::Impl:std::enable_shared_from_this<Impl>{
    struct Link{std::uint64_t client,peer=0;std::size_t index;bool ready=false,opening=false;std::map<std::uint64_t,std::function<void(Status,std::shared_ptr<Link>)>> waiters;};
    struct Session{std::string subject;std::unordered_map<std::size_t,std::shared_ptr<Link>> links;std::uint64_t controlled=0;ConnectionRef connection{};};
    struct Subscription{
        std::uint64_t id,client,remote=0,revision=0,control_epoch=0;std::string type,entity;std::shared_ptr<Link> link;
        bool automatic=false,owned=false,controlled=false,migrating=false,busy=false;
        std::unordered_set<std::size_t> failed;
        Clock::time_point next{},deadline{};
        std::vector<std::function<void()>> detach;
    };
    struct Request{std::uint64_t id,client;std::string subject,data;Reply reply;Clock::time_point deadline;std::size_t bytes;bool done=false;std::function<void()> cancel{};std::vector<std::function<void()>> detach{};Clock::time_point retry_at{};std::function<void()> retry{};std::string resolved_type{};};
    Node& front;Node& back;AsyncDatabase storage;std::vector<EntityEndpoint> endpoints;GatewayLimits limits;GatewayStats counters;
    std::unordered_map<std::uint64_t,Session> sessions;
    std::unordered_map<std::uint64_t,std::shared_ptr<Link>> links;
    std::unordered_map<std::uint64_t,std::shared_ptr<Subscription>> subscriptions;
    std::unordered_map<std::string,std::uint64_t> reverse;
    std::unordered_map<std::uint64_t,std::shared_ptr<Request>> requests;
    struct Lookup { bool busy=false; Clock::time_point expires{}; std::optional<EntityLocation> location; std::map<std::uint64_t,std::function<void(Status,const std::optional<EntityLocation>&)>> waiters; };
    std::unordered_map<std::string,std::shared_ptr<Lookup>> lookups;
    std::unordered_map<std::string,std::unordered_set<std::string>> exposed;
    std::deque<std::uint64_t> schedule;
    bool owns_attributes=false,owns_presence=false,owns_present=false;
    std::vector<Clock::time_point> retry;std::vector<std::string> bound;std::uint64_t serial=0;bool stopped=false,draining=false;
    Impl(Node& f,Node& b,std::shared_ptr<Storage> db,std::vector<EntityEndpoint> e,GatewayLimits l):front(f),back(b),storage(std::move(db)),endpoints(std::move(e)),limits(l){
        validate_entity_endpoints(endpoints);if(&f==&b||!l.clients||!l.links||!l.subscriptions||!l.per_client||!l.requests||!l.bytes||l.request_timeout.count()<=0||l.request_timeout>std::chrono::seconds(30)||l.refresh.count()<=0||l.migration_timeout<l.request_timeout)throw std::invalid_argument("invalid gateway limits");retry.resize(endpoints.size());
    }
    std::string remote_key(std::uint64_t peer,std::uint64_t id){return token(peer)+token(id);}
    bool authenticated(std::uint64_t peer,const std::string& subject){auto who=front.identity(peer);return who&&who->caller==Caller::client&&(who->connection||!subject.empty())&&who->subject==subject;}
    std::chrono::milliseconds remaining(const std::shared_ptr<Request>& r){return std::clamp(std::chrono::duration_cast<std::chrono::milliseconds>(r->deadline-Clock::now()),std::chrono::milliseconds(1),limits.request_timeout);}
    void finish(const std::shared_ptr<Request>& r,Result result){if(std::exchange(r->done,true))return;requests.erase(r->id);r->retry={};counters.queued_bytes-=r->bytes;std::string{}.swap(r->data);auto detach=std::move(r->detach);for(auto& remove:detach)remove();if(result.status!=Status::ok && r->cancel)r->cancel();if(!authenticated(r->client,r->subject))result={Status::disconnected,{}};if(result.status!=Status::ok)++counters.failures;auto reply=std::move(r->reply);reply(std::move(result));}
    std::shared_ptr<Request> request(const CallIdentity& who,std::string data,Reply reply){
        if(stopped){reply({Status::disconnected,{}});return {};}
        if(draining){reply({Status::overloaded,{}});return {};}
        if(!authenticated(who.peer,who.subject)){reply({Status::rejected,{}});return {};}
        if(requests.size()>=limits.requests||data.size()>limits.bytes-counters.queued_bytes||serial>=UINT64_MAX-1){reply({Status::overloaded,{}});return {};}
        auto found=sessions.find(who.peer);if(found==sessions.end()){
            if(sessions.size()>=limits.clients){reply({Status::overloaded,{}});return {};}
            found=sessions.emplace(who.peer,Session{who.subject,{},0,who.connection}).first;
        }
        if(found->second.subject!=who.subject || found->second.connection!=who.connection){front.close(who.peer);reply({Status::rejected,{}});return {};}
        auto r=std::make_shared<Request>(Request{++serial,who.peer,who.subject,std::move(data),std::move(reply),Clock::now()+limits.request_timeout,0});r->bytes=r->data.size();counters.queued_bytes+=r->bytes;requests.emplace(r->id,r);return r;
    }
    void select(std::string type,std::string entity,std::function<void(Status,std::optional<std::size_t>,bool)> done,std::unordered_set<std::size_t> failed={},const std::shared_ptr<Request>& request={},const std::shared_ptr<Subscription>& subscription={}){
        if(!valid_entity_id(entity))return done(Status::rejected,{},false);
        auto ready=[weak=weak_from_this(),type,entity,request,client=request?request->client:(subscription?subscription->client:0),done=std::move(done),failed=std::move(failed)](Status status,const std::optional<EntityLocation>& location){
            auto self=weak.lock();if(!self||self->stopped)return done(Status::disconnected,{},false);
            if(status!=Status::ok)return done(status,{},location.has_value());
            if(!location)return done(Status::not_found,{},false);
            if(!type.empty() && location->type!=type)return done(Status::rejected,{},true);
            const auto& resolved_type=location->type;
            if(request)request->resolved_type=resolved_type;
            auto time=wall();
            // Live leases and directed placement remain authoritative. Only
            // unplaced failover tries other endpoints before retrying failures.
            bool fixed=location->expires_ms>time||!location->placement.empty();
            auto eligible=[&](std::size_t i){
                bool connected=false;auto session=self->sessions.find(client);
                if(session!=self->sessions.end()){auto link=session->second.links.find(i);connected=link!=session->second.links.end() && link->second->ready && self->back.connected(link->second->peer);}
                return (connected || Clock::now()>=self->retry[i]) && (fixed||!failed.contains(i));
            };
            auto index=select_entity_endpoint(self->endpoints,resolved_type,entity,location,time,eligible);
            bool untried=false;
            if(!index&&!fixed&&!failed.empty())for(std::size_t i=0;i<self->endpoints.size();++i){const auto& types=self->endpoints[i].types;if(!failed.contains(i)&&std::find(types.begin(),types.end(),resolved_type)!=types.end()){untried=true;break;}}
            if(!index&&!fixed&&!failed.empty()&&!untried)index=select_entity_endpoint(self->endpoints,resolved_type,entity,location,time,[&](std::size_t i){return Clock::now()>=self->retry[i];});
            done(index?Status::ok:Status::disconnected,index,location.has_value());
        };
        auto key="global:"+entity;auto found=lookups.find(key);
        // A login may create the entity immediately after its routing lookup.
        // Do not let that negative cache revoke the newly announced subscription.
        if(found!=lookups.end()&&!found->second->busy&&found->second->location.has_value()&&Clock::now()<found->second->expires){
            return ready(Status::ok,found->second->location);
        }
        auto wait=[&](const std::shared_ptr<Lookup>& lookup){auto id=request?request->id:subscription->id;lookup->waiters.emplace(id,std::move(ready));auto& detach=request?request->detach:subscription->detach;detach.push_back([weak=std::weak_ptr(lookup),id]{if(auto lookup=weak.lock())lookup->waiters.erase(id);});};
        if(found!=lookups.end()&&found->second->busy){wait(found->second);return;}
        if(lookups.size()>=limits.subscriptions+limits.requests){for(auto it=lookups.begin();it!=lookups.end();){if(!it->second->busy&&Clock::now()>=it->second->expires)it=lookups.erase(it);else ++it;}}
        if(!lookups.contains(key)&&lookups.size()>=limits.subscriptions+limits.requests)return ready(Status::overloaded,{});
        auto lookup=std::make_shared<Lookup>();lookup->busy=true;wait(lookup);lookups[key]=lookup;
        auto location=std::make_shared<std::optional<EntityLocation>>();
        auto complete=[weak=weak_from_this(),lookup,location](std::exception_ptr error){
            auto self=weak.lock();lookup->busy=false;auto waiting=std::move(lookup->waiters);
            if(!self||self->stopped){for(auto& [id,cb]:waiting){(void)id;cb(Status::disconnected,{});}return;}
            lookup->location=*location;lookup->expires=Clock::now()+self->limits.refresh;
            if(error){lookup->expires=Clock::now();for(auto& [id,cb]:waiting){(void)id;cb(Status::internal,*location);}return;}
            for(auto& [id,cb]:waiting){(void)id;cb(Status::ok,*location);}
        };
        if(!storage.submit(type.size()+entity.size(),[location,entity](Storage& db){*location=entity_location(db,{},entity);},complete))complete(std::make_exception_ptr(std::runtime_error("gateway lookup overloaded")));
    }
    void acquire(std::uint64_t client,std::size_t index,std::function<void(Status,std::shared_ptr<Link>)> ready,const std::shared_ptr<Request>& request={},const std::shared_ptr<Subscription>& subscription={}){
        auto session=sessions.find(client);if(session==sessions.end()||!authenticated(client,session->second.subject))return ready(Status::disconnected,{});
        auto found=session->second.links.find(index);
        if(found!=session->second.links.end()&&found->second->ready&&back.connected(found->second->peer))return ready(Status::ok,found->second);
        auto wait=[&](const std::shared_ptr<Link>& link){auto id=request?request->id:subscription->id;link->waiters.emplace(id,std::move(ready));auto& detach=request?request->detach:subscription->detach;detach.push_back([weak=std::weak_ptr(link),id]{if(auto link=weak.lock())link->waiters.erase(id);});};
        if(found!=session->second.links.end()&&found->second->opening){wait(found->second);return;}
        if(links.size()>=limits.links)return ready(Status::overloaded,{});
        auto link=std::make_shared<Link>();link->client=client;link->index=index;link->opening=true;wait(link);
        try{const auto& endpoint=endpoints[index];link->peer=back.connect(endpoint.address,endpoint.port,endpoint.tls_peer_name);session->second.links[index]=link;links[link->peer]=link;
            std::string delegated="FSCN1";snapshot_detail::blob(delegated,session->second.subject);snapshot_detail::blob(delegated,session->second.connection.key());
            back.call(link->peer,"system.gateway.delegate",std::move(delegated),limits.request_timeout,[weak=weak_from_this(),link](Result result){auto self=weak.lock();if(!self)return;link->opening=false;link->ready=result.status==Status::ok;
                auto waiting=std::move(link->waiters);if(!link->ready){self->back.close(link->peer);self->links.erase(link->peer);self->retry[link->index]=Clock::now()+std::chrono::milliseconds(250);}
                for(auto& [id,done]:waiting){(void)id;done(result.status,link->ready?link:nullptr);}
            });
        }catch(...){link->opening=false;auto waiting=std::move(link->waiters);for(auto& [id,done]:waiting){(void)id;done(Status::disconnected,{});}}
    }
    std::size_t client_subscriptions(std::uint64_t client){std::size_t count=0;for(const auto& [id,s]:subscriptions){(void)id;if(s->client==client)++count;}return count;}
    void detach_waiters(const std::shared_ptr<Subscription>& sub){auto detach=std::move(sub->detach);sub->detach.clear();for(auto& remove:detach)remove();}
    void remove(const std::shared_ptr<Subscription>& s,Status status,bool notify=true){
        if(!subscriptions.erase(s->id))return;
        detach_waiters(s);
        std::erase(schedule,s->id);
        auto session=sessions.find(s->client);if(session!=sessions.end()&&session->second.controlled==s->id)session->second.controlled=0;
        if(s->link)reverse.erase(remote_key(s->link->peer,s->remote));
        if(s->link&&s->remote&&back.connected(s->link->peer))back.call(s->link->peer,"system.attributes.unwatch",token(s->remote),std::chrono::seconds(1),[](Result){});
        if(notify&&front.connected(s->client)){
            auto body=encode_replication_packet(s->id,s->type,s->entity,status);std::string channel=attributes;
            if(s->automatic){body=std::string{char(2),char((s->owned?1:0)|(s->controlled?2:0))}+body;channel=presence;}
            if(!front.notify(s->client,channel,body))front.close(s->client);
        }
    }
    void migrating(const std::shared_ptr<Subscription>& s,bool unavailable=false){
        // Repeated dead-link observations must not postpone the next attempt.
        if(s->migrating)return;
        s->migrating=true;s->deadline=Clock::now()+limits.migration_timeout;
        s->next=Clock::time_point{};
        if(unavailable&&s->link){retry[s->link->index]=Clock::now()+std::chrono::milliseconds(250);s->failed.insert(s->link->index);}
    }
    void retry_migration(const std::shared_ptr<Subscription>& sub){
        // Recovery cadence is independent of a long ordinary refresh interval.
        // The failed endpoint set additionally preserves fairness across stalls
        // longer than the transport health backoff.
        sub->next=Clock::now()+std::min(limits.refresh,std::chrono::milliseconds(100));
    }
    void expire_route(const Subscription& sub){
        auto cached=lookups.find("global:"+sub.entity);
        if(cached!=lookups.end())cached->second->expires=Clock::time_point{};
    }
    void routed(const CallIdentity& who,std::string input,Reply reply){
        std::string entity;
        try{auto parsed=parse_entity_call_view(input);if(!parsed)throw std::invalid_argument("invalid entity call");entity=parsed->id.str();}
        catch(...){return reply({Status::rejected,{}});}
        auto r=request(who,std::move(input),std::move(reply));if(!r)return;
        select({},std::move(entity),[weak=weak_from_this(),r](Status status,std::optional<std::size_t> index,bool){auto self=weak.lock();if(!self||r->done)return;if(status!=Status::ok)return self->finish(r,{status,{}});
            {
                auto call=parse_entity_call_view(r->data);auto allowed=self->exposed.find(r->resolved_type);
                if(!call || (!call->expected_type.empty() && call->expected_type!=r->resolved_type) || allowed==self->exposed.end() || !allowed->second.contains(std::string(call->method)))return self->finish(r,{Status::rejected,{}});
            }
            self->acquire(r->client,*index,[weak,r](Status status,std::shared_ptr<Link> link){auto self=weak.lock();if(!self||r->done)return;if(status!=Status::ok)return self->finish(r,{status,{}});++self->counters.forwarded;
                self->back.call(link->peer,entity_call_route,r->data,self->remaining(r),[weak,r](Result result){if(auto self=weak.lock())self->finish(r,std::move(result));});
            },r);
        },{},r);
    }
    void present(const CallIdentity& source,std::string input,Reply reply) {
        auto found=links.find(source.peer);
        if(found==links.end() || !found->second->ready)return reply({Status::rejected,{}});
        auto client=found->second->client;auto identity=front.identity(client);
        if(!identity || !authenticated(client,identity->subject))return reply({Status::disconnected,{}});
        std::string type,id;
        try{std::string_view data(input);auto key=decode_entity_key(data);type=std::move(key.first);id=std::move(key.second);auto action=get(data,1);if(!data.empty() || !valid_entity_id(id) || action<1 || action>4)throw std::invalid_argument("invalid presentation");}
        catch(...){return reply({Status::rejected,{}});}
        auto request=this->request(*identity,std::move(input),std::move(reply));if(!request)return;
        present_attempt(std::move(type),std::move(id),request);
    }
    void present_attempt(std::string type,std::string id,const std::shared_ptr<Request>& request) {
        select(type,id,[weak=weak_from_this(),request,type,id](Status status,std::optional<std::size_t> index,bool){
            auto self=weak.lock();if(!self || request->done)return;
            if(status==Status::disconnected || status==Status::overloaded) {
                request->retry_at=Clock::now()+std::chrono::milliseconds(25);
                request->retry=[weak,request,type,id]{if(auto self=weak.lock())self->present_attempt(type,id,request);};return;
            }
            if(status!=Status::ok)return self->finish(request,{status,{}});
            self->acquire(request->client,*index,[weak,request](Status status,std::shared_ptr<Link> link){
                auto self=weak.lock();if(!self || request->done)return;if(status!=Status::ok)return self->finish(request,{status,{}});
                self->back.call(link->peer,"system.gateway.visibility",request->data,self->remaining(request),[weak,request](Result result){if(auto self=weak.lock())self->finish(request,std::move(result));});
            },request);
        },{},request);
    }
    void watch(const CallIdentity& who,std::string input,Reply reply){
        std::string type,entity;try{std::string_view in(input);auto key=decode_entity_key(in);if(!in.empty() || !valid_entity_id(key.second))throw std::invalid_argument("watch bytes");type=std::move(key.first);entity=std::move(key.second);}catch(...){return reply({Status::rejected,{}});}
        if(subscriptions.size()>=limits.subscriptions||client_subscriptions(who.peer)>=limits.per_client)return reply({Status::overloaded,{}});
        auto r=request(who,std::move(input),std::move(reply));if(!r)return;
        auto sub=std::make_shared<Subscription>();sub->id=++serial;sub->client=who.peer;sub->type=type;sub->entity=entity;sub->busy=true;subscriptions[sub->id]=sub;schedule.push_back(sub->id);
        r->cancel=[weak=weak_from_this(),sub]{if(auto self=weak.lock())self->remove(sub,Status::disconnected,false);};
        select(type,entity,[weak=weak_from_this(),r,sub](Status status,std::optional<std::size_t> index,bool){auto self=weak.lock();if(!self||r->done)return;if(status!=Status::ok){self->remove(sub,status,false);return self->finish(r,{status,{}});}
            self->acquire(r->client,*index,[weak,r,sub](Status status,std::shared_ptr<Link> link){auto self=weak.lock();if(!self||r->done)return;if(status!=Status::ok){self->remove(sub,status,false);return self->finish(r,{status,{}});}
                self->back.call(link->peer,"system.attributes.watch",r->data,self->remaining(r),[weak,r,sub,link](Result result){auto self=weak.lock();if(!self)return;
                    if(r->done){if(result.status==Status::ok)try{auto late=decode_replication_packet_view(result.data);self->back.call(link->peer,"system.attributes.unwatch",token(late.subscription),std::chrono::seconds(1),[](Result){});}catch(...){} return;}
                    try{if(result.status!=Status::ok){self->remove(sub,result.status,false);return self->finish(r,std::move(result));}
                        auto message=decode_replication_packet_view(result.data);if(message.status!=Status::ok||!message.update||!message.update->snapshot||message.type!=sub->type||message.id!=sub->entity)throw std::runtime_error("watch identity mismatch");
                        sub->link=link;sub->remote=message.subscription;sub->revision=message.update->revision;sub->busy=false;sub->next=Clock::now()+self->limits.refresh;self->reverse[self->remote_key(link->peer,sub->remote)]=sub->id;
                        remap_replication_subscription(result.data,sub->id);
                        self->finish(r,{Status::ok,std::move(result.data)});
                    }catch(...){self->remove(sub,Status::rejected,false);self->finish(r,{Status::rejected,{}});}
                });
            },r);
        },{},r);
    }
    void control(const CallIdentity& who,std::string route,std::string input,Reply reply){
        std::uint64_t id;try{std::string_view in(input);id=get(in);if(route!="system.attributes.write"&&!in.empty())throw std::invalid_argument("trailing control");}catch(...){return reply({Status::rejected,{}});}
        auto it=subscriptions.find(id);if(it==subscriptions.end())return reply({Status::not_found,{}});auto sub=it->second;
        if(sub->client!=who.peer||!sessions.contains(who.peer)||sessions.at(who.peer).subject!=who.subject||!authenticated(who.peer,who.subject))return reply({Status::rejected,{}});
        if(route=="system.attributes.unwatch"){remove(sub,Status::disconnected,false);return reply({Status::ok,{}});}
        if(sub->migrating||!sub->link||!back.connected(sub->link->peer)){
            migrating(sub);return reply({route=="system.attributes.renew"?Status::ok:Status::overloaded,{}});
        }
        auto r=request(who,std::move(input),std::move(reply));if(!r)return;
        auto translated=token(sub->remote)+r->data.substr(8);++counters.forwarded;
        back.call(sub->link->peer,route,std::move(translated),remaining(r),[weak=weak_from_this(),r](Result result){if(auto self=weak.lock())self->finish(r,std::move(result));});
    }
    void notice(std::uint64_t peer,const std::string& channel,std::string data,bool confirmed=false){
        auto found=links.find(peer);if(found==links.end()||!found->second->ready)return;auto link=found->second;
        auto session=sessions.find(link->client);if(session==sessions.end()||!authenticated(link->client,session->second.subject))return;
        try{
            std::uint64_t kind=0,flags=0;std::string_view body(data);
            if(channel==presence){kind=get(body,1);flags=get(body,1);if(kind<1||kind>3||flags>3)throw std::invalid_argument("presence header");}
            auto message=decode_replication_packet_view(body);std::shared_ptr<Subscription> sub;
            auto reverse_it=reverse.find(remote_key(peer,message.subscription));
            if(reverse_it!=reverse.end()&&subscriptions.contains(reverse_it->second))sub=subscriptions.at(reverse_it->second);
            if(channel==presence&&kind==1&&message.status==Status::ok&&(!sub||sub->migrating)){
                if(!sub)for(const auto& [id,s]:subscriptions){(void)id;if(s->automatic&&s->migrating&&s->client==link->client&&s->type==message.type&&s->entity==message.id){sub=s;break;}}
                bool restoring=sub&&sub->migrating;
                // show precedes possess/unpossess. Intermediate notifications
                // cannot complete a restore or overwrite its control intent.
                if(restoring&&!confirmed)return;
                if(!sub){if(subscriptions.size()>=limits.subscriptions||client_subscriptions(link->client)>=limits.per_client)throw std::length_error("gateway subscription limit");sub=std::make_shared<Subscription>();sub->id=++serial;sub->client=link->client;sub->type=message.type;sub->entity=message.id;sub->automatic=true;sub->owned=flags&1;sub->controlled=flags&2;subscriptions[sub->id]=sub;schedule.push_back(sub->id);}
                if(!message.update||!message.update->snapshot||message.update->revision<sub->revision||sub->owned!=bool(flags&1))return remove(sub,Status::rejected);
                if(sub->link)reverse.erase(remote_key(sub->link->peer,sub->remote));
                sub->link=link;sub->remote=message.subscription;sub->revision=message.update->revision;sub->controlled=flags&2;sub->migrating=false;sub->busy=false;sub->failed.clear();sub->next=Clock::now()+limits.refresh;reverse[remote_key(peer,sub->remote)]=sub->id;
                // Restoring may have acquired an expired lease on a new owner.
                // The lookup that selected it can still contain the old owner;
                // expire it before the client sends an immediate follow-up RPC.
                expire_route(*sub);
                if(restoring){++counters.migrations;remap_replication_subscription(data,sub->id,2);if(!front.notify(sub->client,attributes,body))return front.close(sub->client);}
            }
            if(!sub)return;
            if(message.type!=sub->type||message.id!=sub->entity)throw std::invalid_argument("notification entity mismatch");
            if(message.status!=Status::ok){if(message.status==Status::disconnected){migrating(sub);return;}return remove(sub,message.status);}
            if(sub->migrating)return;
            if(channel==attributes){if(!message.update)throw std::invalid_argument("missing update");if(message.update->snapshot){if(message.update->revision<sub->revision)throw std::invalid_argument("stale snapshot");}else if(message.update->base!=sub->revision)throw std::invalid_argument("revision gap");sub->revision=message.update->revision;}
            else {
                if(flags&2){
                    auto old_id=session->second.controlled;
                    if(old_id&&old_id!=sub->id&&subscriptions.contains(old_id)){
                        auto old=subscriptions.at(old_id);old->controlled=false;++old->control_epoch;
                        AttributeUpdate state{0,old->revision,true,{}};
                        auto previous=std::string{char(3),char(old->owned?1:0)}+encode_replication_packet(old->id,old->type,old->entity,Status::ok,&state,30000);
                        if(!front.notify(old->client,presence,previous))return front.close(old->client);
                        if(old->link&&back.connected(old->link->peer))back.call(old->link->peer,"system.gateway.unpossess",encode_entity_key(old->type,old->entity),limits.request_timeout,[weak=weak_from_this(),old,epoch=old->control_epoch](Result r){
                            if(r.status==Status::ok||r.status==Status::not_found)return;
                            if(auto self=weak.lock();self&&self->subscriptions.contains(old->id)&&old->control_epoch==epoch){self->migrating(old);self->expire_route(*old);self->retry_migration(old);}
                        });
                    }
                    session->second.controlled=sub->id;
                }else if(session->second.controlled==sub->id)session->second.controlled=0;
                if(sub->controlled!=bool(flags&2))++sub->control_epoch;
                sub->controlled=flags&2;
            }
            remap_replication_subscription(data,sub->id,channel==presence?2:0);
            if(!front.notify(sub->client,channel,data))front.close(sub->client);
        }catch(...){front.close(link->client);}
    }
    void refresh(const std::shared_ptr<Subscription>& sub){
        if(sub->busy)return;
        detach_waiters(sub);
        sub->busy=true;sub->next=Clock::now()+limits.refresh;
        select(sub->type,sub->entity,[weak=weak_from_this(),sub](Status status,std::optional<std::size_t> index,bool exists){auto self=weak.lock();if(!self||!self->subscriptions.contains(sub->id))return;
            self->detach_waiters(sub);
            if(!exists&&status!=Status::internal&&status!=Status::overloaded){sub->busy=false;return self->remove(sub,Status::not_found);}
            if(status!=Status::ok){sub->busy=false;self->migrating(sub);self->retry_migration(sub);return;}
            if(sub->link&&sub->link->index==*index&&self->back.connected(sub->link->peer)&&!sub->migrating){sub->busy=false;return;}
            self->migrating(sub);
            if(sub->failed.contains(*index))sub->failed.clear(); // every compatible endpoint was tried; begin another round
            self->acquire(sub->client,*index,[weak,sub,index](Status status,std::shared_ptr<Link> link){auto self=weak.lock();if(!self||!self->subscriptions.contains(sub->id))return;
                self->detach_waiters(sub);
                if(status!=Status::ok){sub->busy=false;sub->failed.insert(*index);self->retry_migration(sub);return;}
                auto control=sub->controlled;auto epoch=sub->control_epoch;
                std::string route=sub->automatic?"system.gateway.restore":"system.attributes.watch";auto input=encode_entity_key(sub->type,sub->entity);if(sub->automatic)put(input,control,1);
                self->back.call(link->peer,route,std::move(input),self->limits.request_timeout,[weak,sub,link,control,epoch](Result result){auto self=weak.lock();if(!self||!self->subscriptions.contains(sub->id))return;
                    sub->busy=false;sub->next=Clock::now()+self->limits.refresh;
                    // A valid new possession can supersede an in-flight
                    // restore. Reconcile the latest intent on the new owner
                    // before publishing its snapshot or control flags.
                    if(sub->automatic&&sub->control_epoch!=epoch){sub->next=Clock::time_point{};self->expire_route(*sub);return;}
                    if(result.status!=Status::ok){if(result.status==Status::overloaded||result.status==Status::disconnected||result.status==Status::timeout){self->retry[link->index]=Clock::now()+std::chrono::milliseconds(250);sub->failed.insert(link->index);}if(result.status==Status::rejected||result.status==Status::not_found)self->remove(sub,result.status);else self->retry_migration(sub);return;}
                    if(sub->automatic){
                        try{
                            std::string_view body(result.data);auto kind=get(body,1),flags=get(body,1);
                            auto message=decode_replication_packet_view(body);
                            if(kind!=1||flags>3||bool(flags&2)!=control||message.type!=sub->type||message.id!=sub->entity||
                               message.status!=Status::ok||!message.update||!message.update->snapshot)
                                throw std::invalid_argument("restore confirmation");
                            self->notice(link->peer,presence,std::move(result.data),true);
                        }catch(...){self->remove(sub,Status::rejected);}
                        return;
                    }
                    try{auto message=decode_replication_packet_view(result.data);if(!message.update||!message.update->snapshot||message.type!=sub->type||message.id!=sub->entity||message.update->revision<sub->revision)throw std::invalid_argument("migration snapshot");
                        if(sub->link){self->reverse.erase(self->remote_key(sub->link->peer,sub->remote));if(self->back.connected(sub->link->peer))self->back.call(sub->link->peer,"system.attributes.unwatch",token(sub->remote),std::chrono::seconds(1),[](Result){});}
                        sub->link=link;sub->remote=message.subscription;sub->revision=message.update->revision;sub->migrating=false;sub->failed.clear();self->reverse[self->remote_key(link->peer,sub->remote)]=sub->id;++self->counters.migrations;
                        self->expire_route(*sub);
                        remap_replication_subscription(result.data,sub->id);
                        if(!self->front.notify(sub->client,attributes,result.data))self->front.close(sub->client);
                    }catch(...){self->remove(sub,Status::rejected);}
                });
            },{},sub);
        },sub->failed,{},sub);
    }
    void stop(){if(std::exchange(stopped,true))return;for(const auto& r:bound)front.unbind(r);if(owns_attributes)back.off_notification(attributes);if(owns_presence)back.off_notification(presence);if(owns_present)back.unbind("system.gateway.present");
        auto req=requests;for(auto& [id,r]:req){(void)id;finish(r,{Status::disconnected,{}});}auto subs=subscriptions;for(auto& [id,s]:subs){(void)id;remove(s,Status::disconnected);}
        auto copy=links;for(auto& [peer,l]:copy){(void)l;back.close(peer);}links.clear();sessions.clear();storage.stop();}
};
ClientGateway::ClientGateway(Node& front,Node& back,std::shared_ptr<Storage> db,std::vector<EntityEndpoint> endpoints,GatewayLimits limits):impl_(std::make_shared<Impl>(front,back,std::move(db),std::move(endpoints),limits)){
    auto weak=std::weak_ptr(impl_);
    for(const auto* name:{"system.attributes.watch","system.attributes.unwatch","system.attributes.renew","system.attributes.write"})if(front.has_route(name))throw std::logic_error("gateway attribute routes already bound");
    if(back.has_route("system.gateway.present"))throw std::logic_error("gateway presentation route already bound");
    try{
        back.bind_rpc("system.gateway.present",{true,true,false},[weak](const CallIdentity& who,std::string data,Reply reply){if(auto self=weak.lock())self->present(who,std::move(data),std::move(reply));else reply({Status::disconnected,{}});});impl_->owns_present=true;
        front.bind_rpc(entity_call_route,{true,false,false},[weak](const CallIdentity& who,std::string data,Reply reply){
            if(auto self=weak.lock())self->routed(who,std::move(data),std::move(reply));else reply({Status::disconnected,{}});
        });impl_->bound.emplace_back(entity_call_route);
        for(const auto* name:{"system.attributes.watch","system.attributes.unwatch","system.attributes.renew","system.attributes.write"}){
        std::string route=name;front.bind_rpc(route,{true,false,false},[weak,route](const CallIdentity& who,std::string data,Reply reply){auto self=weak.lock();if(!self)return reply({Status::disconnected,{}});if(route=="system.attributes.watch")self->watch(who,std::move(data),std::move(reply));else self->control(who,route,std::move(data),std::move(reply));});impl_->bound.push_back(route);}
        back.on_notification(attributes,[weak](std::uint64_t peer,const std::string&,std::string data){if(auto self=weak.lock())self->notice(peer,attributes,std::move(data));});impl_->owns_attributes=true;
        back.on_notification(presence,[weak](std::uint64_t peer,const std::string&,std::string data){if(auto self=weak.lock())self->notice(peer,presence,std::move(data));});impl_->owns_presence=true;
    }catch(...){impl_->stop();throw;}
}
ClientGateway::~ClientGateway(){stop();}
void ClientGateway::bind_entity(std::string type,std::vector<std::string> methods){
    (void)entity_route(type,"validate");
    for(const auto& method:methods)if(!valid_entity_method(method))throw std::invalid_argument("invalid exposed method");
    for(auto& method:methods)impl_->exposed[type].insert(std::move(method));
}
void ClientGateway::poll(std::size_t budget){auto self=impl_;if(self->stopped)return;self->storage.poll(budget);auto now=Clock::now();std::vector<std::uint64_t> dead;
    for(const auto& [peer,s]:self->sessions)if(!self->authenticated(peer,s.subject))dead.push_back(peer);
    for(auto peer:dead){self->front.close(peer);auto subs=self->subscriptions;for(auto& [id,s]:subs){(void)id;if(s->client==peer)self->remove(s,Status::disconnected,false);}auto it=self->sessions.find(peer);if(it!=self->sessions.end()){for(auto& [index,l]:it->second.links){(void)index;self->back.close(l->peer);self->links.erase(l->peer);}self->sessions.erase(it);}}
    std::vector<std::uint64_t> lost;
    for(const auto& [peer,l]:self->links)if(!l->opening&&!self->back.connected(peer))lost.push_back(peer);
    for(auto peer:lost){auto l=self->links.at(peer);auto it=self->sessions.find(l->client);if(it!=self->sessions.end()){auto found=it->second.links.find(l->index);if(found!=it->second.links.end()&&found->second==l)it->second.links.erase(found);}self->links.erase(peer);}
    auto req=self->requests;for(auto& [id,r]:req){(void)id;if(now>=r->deadline)self->finish(r,{Status::timeout,{}});else if(!self->authenticated(r->client,r->subject))self->finish(r,{Status::disconnected,{}});else if(r->retry && now>=r->retry_at){auto retry=std::move(r->retry);r->retry={};retry();}}
    auto count=self->schedule.size();std::size_t work=0;
    while(count-- && work++<budget && !self->schedule.empty()){
        auto id=self->schedule.front();self->schedule.pop_front();auto it=self->subscriptions.find(id);if(it==self->subscriptions.end())continue;auto s=it->second;self->schedule.push_back(id);
        if(s->link&&s->link->ready&&!self->back.connected(s->link->peer))self->migrating(s,true);
        if(s->migrating&&now>=s->deadline)self->remove(s,Status::timeout);else if(!s->busy&&now>=s->next)self->refresh(s);
    }
}
GatewayStats ClientGateway::stats()const{auto r=impl_->counters;r.clients=impl_->sessions.size();r.links=impl_->links.size();r.subscriptions=impl_->subscriptions.size();r.pending=impl_->requests.size();for(const auto& [id,s]:impl_->subscriptions){(void)id;if(s->migrating)++r.migrating;}for(const auto& [key,l]:impl_->lookups){(void)key;r.lookup_waiters+=l->waiters.size();}for(const auto& [id,l]:impl_->links){(void)id;r.link_waiters+=l->waiters.size();}return r;}
void ClientGateway::drain(){impl_->draining=true;}
void ClientGateway::stop(){impl_->stop();}
}

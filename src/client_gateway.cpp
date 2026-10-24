#include "fsnet/client_gateway.hpp"
#include "fsnet/entity_message.hpp"
#include "fsnet/replication_wire.hpp"
#include <algorithm>
#include <deque>
#include <unordered_map>
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
    struct Session{std::string subject,transport,certificate;};
    Node& node;Node::IdentityResolver base;Trust trust;Restore restore;std::shared_ptr<EntityScriptHost> host;
    std::unordered_map<std::uint64_t,Session> sessions;std::size_t limit;bool stopped=false;
    Impl(Node& n,Node::IdentityResolver b,Trust t,std::shared_ptr<EntityScriptHost> h,Restore r,std::size_t l):node(n),base(std::move(b)),trust(std::move(t)),restore(std::move(r)),host(std::move(h)),limit(l){if(!base||!trust||!host||!limit)throw std::invalid_argument("gateway trust and entity host required");}
    bool valid(std::uint64_t peer,const Session& s,const std::string& cert){
        auto identity=base(peer,cert);return identity && identity->caller==Caller::server && identity->subject==s.transport && cert==s.certificate && trust(*identity,cert);
    }
    void stop(){if(std::exchange(stopped,true))return;node.unbind("system.gateway.delegate");node.unbind("system.gateway.restore");node.unbind("system.gateway.unpossess");auto copy=std::move(sessions);for(const auto& [peer,s]:copy){(void)s;node.close(peer);}node.identify(base);}
};
GatewayDelegation::GatewayDelegation(Node& node,Node::IdentityResolver base,Trust trust,std::shared_ptr<EntityScriptHost> host,Restore restore,std::size_t limit):impl_(std::make_shared<Impl>(node,std::move(base),std::move(trust),std::move(host),std::move(restore),limit)){
    if(node.has_route("system.gateway.delegate")||node.has_route("system.gateway.restore")||node.has_route("system.gateway.unpossess"))throw std::logic_error("gateway routes already bound");
    auto weak=std::weak_ptr(impl_);
    try{
        node.bind_rpc("system.gateway.delegate",{false,true,false},[weak](const CallIdentity& who,std::string subject,Reply reply){
            auto self=weak.lock();if(!self||self->stopped)return reply({Status::disconnected,{}});
            auto cert=self->node.peer_identity(who.peer);
            try{
                if(subject.empty()||subject.size()>256||!self->trust(who,cert)||self->sessions.contains(who.peer))return reply({Status::rejected,{}});
                if(self->sessions.size()>=self->limit)return reply({Status::overloaded,{}});
                self->sessions.emplace(who.peer,Impl::Session{std::move(subject),who.subject,std::move(cert)});reply({Status::ok,{}});
            }catch(...){reply({Status::rejected,{}});}
        });
        node.bind_rpc("system.gateway.restore",{true,false,false},[weak](const CallIdentity& who,std::string data,Reply reply){
            auto self=weak.lock();if(!self||self->stopped)return reply({Status::disconnected,{}});
            try{
                if(!self->sessions.contains(who.peer))return reply({Status::rejected,{}});
                std::string_view in(data);auto [type,id]=decode_entity_key(in);auto control=get(in,1);if(!in.empty()||control>1)throw std::invalid_argument("invalid restore");
                bool allowed=self->host->owner_subject(type,id)==who.subject;
                if(self->restore)allowed=self->restore(who,type,id,control!=0);
                if(!allowed)return reply({Status::rejected,{}});
                self->host->read_attributes(type,id,[weak,who,type,id,control,reply=std::move(reply)](Result result)mutable{
                    auto self=weak.lock();if(!self||self->stopped)return reply({Status::disconnected,{}});
                    if(result.status!=Status::ok)return reply(std::move(result));
                    auto identity=self->node.identity(who.peer);if(!identity||identity->caller!=Caller::client||identity->subject!=who.subject)return reply({Status::rejected,{}});
                    self->host->entity_visibility(who.peer,type,id,1,[weak,who,type,id,control,reply=std::move(reply)](Result result)mutable{
                        auto self=weak.lock();if(!self||self->stopped)return reply({Status::disconnected,{}});
                        if(result.status!=Status::ok||!control)return reply(std::move(result));
                        self->host->entity_visibility(who.peer,type,id,3,std::move(reply));
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
            return CallIdentity{Caller::client,it->second.subject,peer};
        });
    }catch(...){impl_->stop();throw;}
}
GatewayDelegation::~GatewayDelegation(){stop();}
void GatewayDelegation::poll(){auto self=impl_;std::vector<std::uint64_t> dead;for(const auto& [peer,s]:self->sessions)try{if(!self->node.connected(peer)||!self->valid(peer,s,self->node.peer_identity(peer)))dead.push_back(peer);}catch(...){dead.push_back(peer);}for(auto peer:dead){self->node.close(peer);self->sessions.erase(peer);}}
void GatewayDelegation::stop(){impl_->stop();}

struct ClientGateway::Impl:std::enable_shared_from_this<Impl>{
    struct Link{std::uint64_t client,peer=0;std::size_t index;bool ready=false,opening=false;std::vector<std::function<void(Status,std::shared_ptr<Link>)>> waiters;};
    struct Session{std::string subject;std::unordered_map<std::size_t,std::shared_ptr<Link>> links;std::uint64_t controlled=0;};
    struct Subscription{
        std::uint64_t id,client,remote=0,revision=0;std::string type,entity;std::shared_ptr<Link> link;
        bool automatic=false,owned=false,controlled=false,migrating=false,busy=false;
        Clock::time_point next{},deadline{};
    };
    struct Request{std::uint64_t id,client;std::string subject,data;Reply reply;Clock::time_point deadline;std::size_t bytes;bool done=false;std::function<void()> cancel{};};
    Node& front;Node& back;AsyncDatabase storage;std::vector<EntityEndpoint> endpoints;GatewayLimits limits;GatewayStats counters;
    std::unordered_map<std::uint64_t,Session> sessions;
    std::unordered_map<std::uint64_t,std::shared_ptr<Link>> links;
    std::unordered_map<std::uint64_t,std::shared_ptr<Subscription>> subscriptions;
    std::unordered_map<std::string,std::uint64_t> reverse;
    std::unordered_map<std::uint64_t,std::shared_ptr<Request>> requests;
    struct Lookup { bool busy=false; Clock::time_point expires{}; std::optional<EntityLocation> location; std::vector<std::function<void(Status,std::optional<std::size_t>,bool)>> waiters; };
    std::unordered_map<std::string,std::shared_ptr<Lookup>> lookups;
    std::deque<std::uint64_t> schedule;
    bool owns_attributes=false,owns_presence=false;
    std::vector<Clock::time_point> retry;std::vector<std::string> bound;std::uint64_t serial=0;bool stopped=false,draining=false;
    Impl(Node& f,Node& b,std::shared_ptr<Database> db,std::vector<EntityEndpoint> e,GatewayLimits l):front(f),back(b),storage(std::move(db)),endpoints(std::move(e)),limits(l){
        validate_entity_endpoints(endpoints);if(&f==&b||!l.clients||!l.links||!l.subscriptions||!l.per_client||!l.requests||!l.bytes||l.request_timeout.count()<=0||l.request_timeout>std::chrono::seconds(30)||l.refresh.count()<=0||l.migration_timeout<l.request_timeout)throw std::invalid_argument("invalid gateway limits");retry.resize(endpoints.size());
    }
    std::string remote_key(std::uint64_t peer,std::uint64_t id){return token(peer)+token(id);}
    bool authenticated(std::uint64_t peer,const std::string& subject){auto who=front.identity(peer);return who&&who->caller==Caller::client&&!subject.empty()&&who->subject==subject;}
    std::chrono::milliseconds remaining(const std::shared_ptr<Request>& r){return std::clamp(std::chrono::duration_cast<std::chrono::milliseconds>(r->deadline-Clock::now()),std::chrono::milliseconds(1),limits.request_timeout);}
    void finish(const std::shared_ptr<Request>& r,Result result){if(std::exchange(r->done,true))return;requests.erase(r->id);counters.queued_bytes-=r->bytes;r->data.clear();if(result.status!=Status::ok && r->cancel)r->cancel();if(!authenticated(r->client,r->subject))result={Status::disconnected,{}};if(result.status!=Status::ok)++counters.failures;auto reply=std::move(r->reply);reply(std::move(result));}
    std::shared_ptr<Request> request(const CallIdentity& who,std::string data,Reply reply){
        if(stopped){reply({Status::disconnected,{}});return {};}
        if(draining){reply({Status::overloaded,{}});return {};}
        if(!authenticated(who.peer,who.subject)){reply({Status::rejected,{}});return {};}
        if(requests.size()>=limits.requests||data.size()>limits.bytes-counters.queued_bytes||serial>=UINT64_MAX-1){reply({Status::overloaded,{}});return {};}
        auto found=sessions.find(who.peer);if(found==sessions.end()){
            if(sessions.size()>=limits.clients){reply({Status::overloaded,{}});return {};}
            found=sessions.emplace(who.peer,Session{who.subject,{}}).first;
        }
        if(found->second.subject!=who.subject){front.close(who.peer);reply({Status::rejected,{}});return {};}
        auto r=std::make_shared<Request>(Request{++serial,who.peer,who.subject,std::move(data),std::move(reply),Clock::now()+limits.request_timeout,0});r->bytes=r->data.size();counters.queued_bytes+=r->bytes;requests.emplace(r->id,r);return r;
    }
    void select(std::string type,std::string entity,std::function<void(Status,std::optional<std::size_t>,bool)> done){
        auto key=encode_entity_key(type,entity);auto found=lookups.find(key);
        // A login may create the entity immediately after its routing lookup.
        // Do not let that negative cache revoke the newly announced subscription.
        if(found!=lookups.end()&&!found->second->busy&&found->second->location.has_value()&&Clock::now()<found->second->expires){
            auto index=select_entity_endpoint(endpoints,type,entity,found->second->location,wall(),[&](std::size_t i){return Clock::now()>=retry[i];});
            return done(index?Status::ok:Status::disconnected,index,found->second->location.has_value());
        }
        if(found!=lookups.end()&&found->second->busy){found->second->waiters.push_back(std::move(done));return;}
        if(lookups.size()>=limits.subscriptions+limits.requests){for(auto it=lookups.begin();it!=lookups.end();){if(!it->second->busy&&Clock::now()>=it->second->expires)it=lookups.erase(it);else ++it;}}
        if(!lookups.contains(key)&&lookups.size()>=limits.subscriptions+limits.requests)return done(Status::overloaded,{},false);
        auto lookup=std::make_shared<Lookup>();lookup->busy=true;lookup->waiters.push_back(std::move(done));lookups[key]=lookup;
        auto location=std::make_shared<std::optional<EntityLocation>>();
        auto complete=[weak=weak_from_this(),lookup,location,type,entity](std::exception_ptr error){
            auto self=weak.lock();lookup->busy=false;auto waiting=std::move(lookup->waiters);
            if(!self||self->stopped){for(auto& cb:waiting)cb(Status::disconnected,{},false);return;}
            lookup->location=*location;lookup->expires=Clock::now()+self->limits.refresh;
            if(error){lookup->expires=Clock::now();for(auto& cb:waiting)cb(Status::internal,{},false);return;}
            auto index=select_entity_endpoint(self->endpoints,type,entity,*location,wall(),[&](std::size_t i){return Clock::now()>=self->retry[i];});
            for(auto& cb:waiting)cb(index?Status::ok:Status::disconnected,index,location->has_value());
        };
        if(!storage.submit(type.size()+entity.size(),[location,type,entity](Database& db){*location=entity_location(db,type,entity);},complete))complete(std::make_exception_ptr(std::runtime_error("gateway lookup overloaded")));
    }
    void acquire(std::uint64_t client,std::size_t index,std::function<void(Status,std::shared_ptr<Link>)> ready){
        auto session=sessions.find(client);if(session==sessions.end()||!authenticated(client,session->second.subject))return ready(Status::disconnected,{});
        auto found=session->second.links.find(index);
        if(found!=session->second.links.end()&&found->second->ready&&back.connected(found->second->peer))return ready(Status::ok,found->second);
        if(found!=session->second.links.end()&&found->second->opening){found->second->waiters.push_back(std::move(ready));return;}
        if(links.size()>=limits.links)return ready(Status::overloaded,{});
        auto link=std::make_shared<Link>();link->client=client;link->index=index;link->opening=true;link->waiters.push_back(std::move(ready));
        try{const auto& endpoint=endpoints[index];link->peer=back.connect(endpoint.address,endpoint.port,endpoint.tls_peer_name);session->second.links[index]=link;links[link->peer]=link;
            back.call(link->peer,"system.gateway.delegate",session->second.subject,limits.request_timeout,[weak=weak_from_this(),link](Result result){auto self=weak.lock();if(!self)return;link->opening=false;link->ready=result.status==Status::ok;
                auto waiting=std::move(link->waiters);if(!link->ready){self->back.close(link->peer);self->links.erase(link->peer);self->retry[link->index]=Clock::now()+std::chrono::milliseconds(250);}
                for(auto& done:waiting)done(result.status,link->ready?link:nullptr);
            });
        }catch(...){link->opening=false;auto waiting=std::move(link->waiters);for(auto& done:waiting)done(Status::disconnected,{});}
    }
    std::size_t client_subscriptions(std::uint64_t client){std::size_t count=0;for(const auto& [id,s]:subscriptions){(void)id;if(s->client==client)++count;}return count;}
    void remove(const std::shared_ptr<Subscription>& s,Status status,bool notify=true){
        if(!subscriptions.erase(s->id))return;
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
        if(unavailable&&s->link)retry[s->link->index]=Clock::now()+std::chrono::milliseconds(250);
    }
    void routed(const CallIdentity& who,std::string type,std::string route,std::string input,bool account,Reply reply){
        std::string entity;
        try{if(account)entity=who.subject;else{auto parsed=parse_entity_message_view(input);if(!parsed)throw std::invalid_argument("invalid entity request");entity=parsed->id;}}catch(...){return reply({Status::rejected,{}});}
        auto r=request(who,std::move(input),std::move(reply));if(!r)return;
        select(std::move(type),std::move(entity),[weak=weak_from_this(),r,route](Status status,std::optional<std::size_t> index,bool){auto self=weak.lock();if(!self||r->done)return;if(status!=Status::ok)return self->finish(r,{status,{}});
            self->acquire(r->client,*index,[weak,r,route](Status status,std::shared_ptr<Link> link){auto self=weak.lock();if(!self||r->done)return;if(status!=Status::ok)return self->finish(r,{status,{}});++self->counters.forwarded;
                self->back.call(link->peer,route,r->data,self->remaining(r),[weak,r](Result result){if(auto self=weak.lock())self->finish(r,std::move(result));});
            });
        });
    }
    void watch(const CallIdentity& who,std::string input,Reply reply){
        std::string type,entity;try{std::string_view in(input);auto key=decode_entity_key(in);if(!in.empty())throw std::invalid_argument("watch bytes");type=std::move(key.first);entity=std::move(key.second);}catch(...){return reply({Status::rejected,{}});}
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
            });
        });
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
    void notice(std::uint64_t peer,const std::string& channel,std::string data){
        auto found=links.find(peer);if(found==links.end()||!found->second->ready)return;auto link=found->second;
        auto session=sessions.find(link->client);if(session==sessions.end()||!authenticated(link->client,session->second.subject))return;
        try{
            std::uint64_t kind=0,flags=0;std::string_view body(data);
            if(channel==presence){kind=get(body,1);flags=get(body,1);if(kind<1||kind>3||flags>3)throw std::invalid_argument("presence header");}
            auto message=decode_replication_packet_view(body);std::shared_ptr<Subscription> sub;
            auto reverse_it=reverse.find(remote_key(peer,message.subscription));
            if(reverse_it!=reverse.end()&&subscriptions.contains(reverse_it->second))sub=subscriptions.at(reverse_it->second);
            if(!sub&&channel==presence&&kind==1&&message.status==Status::ok){
                for(const auto& [id,s]:subscriptions){(void)id;if(s->automatic&&s->migrating&&s->client==link->client&&s->type==message.type&&s->entity==message.id){sub=s;break;}}
                bool restoring=bool(sub);
                if(!sub){if(subscriptions.size()>=limits.subscriptions||client_subscriptions(link->client)>=limits.per_client)throw std::length_error("gateway subscription limit");sub=std::make_shared<Subscription>();sub->id=++serial;sub->client=link->client;sub->type=message.type;sub->entity=message.id;sub->automatic=true;sub->owned=flags&1;sub->controlled=flags&2;subscriptions[sub->id]=sub;schedule.push_back(sub->id);}
                if(!message.update||!message.update->snapshot||message.update->revision<sub->revision||sub->owned!=bool(flags&1))return remove(sub,Status::rejected);
                if(sub->link)reverse.erase(remote_key(sub->link->peer,sub->remote));
                sub->link=link;sub->remote=message.subscription;sub->revision=message.update->revision;sub->controlled=flags&2;sub->migrating=false;sub->busy=false;sub->next=Clock::now()+limits.refresh;reverse[remote_key(peer,sub->remote)]=sub->id;
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
                        auto old=subscriptions.at(old_id);old->controlled=false;
                        AttributeUpdate state{0,old->revision,true,{}};
                        auto previous=std::string{char(3),char(old->owned?1:0)}+encode_replication_packet(old->id,old->type,old->entity,Status::ok,&state,30000);
                        if(!front.notify(old->client,presence,previous))return front.close(old->client);
                        if(old->link&&back.connected(old->link->peer))back.call(old->link->peer,"system.gateway.unpossess",encode_entity_key(old->type,old->entity),limits.request_timeout,[weak=weak_from_this(),client=old->client](Result r){if(r.status!=Status::ok)if(auto self=weak.lock())self->front.close(client);});
                    }
                    session->second.controlled=sub->id;
                }else if(session->second.controlled==sub->id)session->second.controlled=0;
                sub->controlled=flags&2;
            }
            remap_replication_subscription(data,sub->id,channel==presence?2:0);
            if(!front.notify(sub->client,channel,data))front.close(sub->client);
        }catch(...){front.close(link->client);}
    }
    void refresh(const std::shared_ptr<Subscription>& sub){
        if(sub->busy)return;
        sub->busy=true;sub->next=Clock::now()+limits.refresh;
        select(sub->type,sub->entity,[weak=weak_from_this(),sub](Status status,std::optional<std::size_t> index,bool exists){auto self=weak.lock();if(!self||!self->subscriptions.contains(sub->id))return;
            if(!exists&&status!=Status::internal&&status!=Status::overloaded){sub->busy=false;return self->remove(sub,Status::not_found);}
            if(status!=Status::ok){sub->busy=false;self->migrating(sub);sub->next=Clock::now()+self->limits.refresh;return;}
            if(sub->link&&sub->link->index==*index&&self->back.connected(sub->link->peer)&&!sub->migrating){sub->busy=false;return;}
            self->migrating(sub);
            self->acquire(sub->client,*index,[weak,sub](Status status,std::shared_ptr<Link> link){auto self=weak.lock();if(!self||!self->subscriptions.contains(sub->id))return;
                if(status!=Status::ok){sub->busy=false;sub->next=Clock::now()+self->limits.refresh;return;}
                std::string route=sub->automatic?"system.gateway.restore":"system.attributes.watch";auto input=encode_entity_key(sub->type,sub->entity);if(sub->automatic)put(input,sub->controlled,1);
                self->back.call(link->peer,route,std::move(input),self->limits.request_timeout,[weak,sub,link](Result result){auto self=weak.lock();if(!self||!self->subscriptions.contains(sub->id))return;
                    sub->busy=false;sub->next=Clock::now()+self->limits.refresh;
                    if(result.status!=Status::ok){if(result.status==Status::overloaded||result.status==Status::disconnected||result.status==Status::timeout)self->retry[link->index]=Clock::now()+std::chrono::milliseconds(250);if(result.status==Status::rejected||result.status==Status::not_found)self->remove(sub,result.status);return;}
                    if(sub->automatic)return; // creation notification installs the fresh snapshot
                    try{auto message=decode_replication_packet_view(result.data);if(!message.update||!message.update->snapshot||message.type!=sub->type||message.id!=sub->entity||message.update->revision<sub->revision)throw std::invalid_argument("migration snapshot");
                        if(sub->link){self->reverse.erase(self->remote_key(sub->link->peer,sub->remote));if(self->back.connected(sub->link->peer))self->back.call(sub->link->peer,"system.attributes.unwatch",token(sub->remote),std::chrono::seconds(1),[](Result){});}
                        sub->link=link;sub->remote=message.subscription;sub->revision=message.update->revision;sub->migrating=false;self->reverse[self->remote_key(link->peer,sub->remote)]=sub->id;++self->counters.migrations;
                        remap_replication_subscription(result.data,sub->id);
                        if(!self->front.notify(sub->client,attributes,result.data))self->front.close(sub->client);
                    }catch(...){self->remove(sub,Status::rejected);}
                });
            });
        });
    }
    void stop(){if(std::exchange(stopped,true))return;for(const auto& r:bound)front.unbind(r);if(owns_attributes)back.off_notification(attributes);if(owns_presence)back.off_notification(presence);
        auto req=requests;for(auto& [id,r]:req){(void)id;finish(r,{Status::disconnected,{}});}auto subs=subscriptions;for(auto& [id,s]:subs){(void)id;remove(s,Status::disconnected);}
        auto copy=links;for(auto& [peer,l]:copy){(void)l;back.close(peer);}links.clear();sessions.clear();storage.stop();}
};
ClientGateway::ClientGateway(Node& front,Node& back,std::shared_ptr<Database> db,std::vector<EntityEndpoint> endpoints,GatewayLimits limits):impl_(std::make_shared<Impl>(front,back,std::move(db),std::move(endpoints),limits)){
    auto weak=std::weak_ptr(impl_);
    for(const auto* name:{"system.attributes.watch","system.attributes.unwatch","system.attributes.renew","system.attributes.write"})if(front.has_route(name))throw std::logic_error("gateway attribute routes already bound");
    try{for(const auto* name:{"system.attributes.watch","system.attributes.unwatch","system.attributes.renew","system.attributes.write"}){
        std::string route=name;front.bind_rpc(route,{true,false,false},[weak,route](const CallIdentity& who,std::string data,Reply reply){auto self=weak.lock();if(!self)return reply({Status::disconnected,{}});if(route=="system.attributes.watch")self->watch(who,std::move(data),std::move(reply));else self->control(who,route,std::move(data),std::move(reply));});impl_->bound.push_back(route);}
        back.on_notification(attributes,[weak](std::uint64_t peer,const std::string&,std::string data){if(auto self=weak.lock())self->notice(peer,attributes,std::move(data));});impl_->owns_attributes=true;
        back.on_notification(presence,[weak](std::uint64_t peer,const std::string&,std::string data){if(auto self=weak.lock())self->notice(peer,presence,std::move(data));});impl_->owns_presence=true;
    }catch(...){impl_->stop();throw;}
}
ClientGateway::~ClientGateway(){stop();}
void ClientGateway::bind_entity(std::string type,std::vector<std::string> methods){
    for(const auto& method:methods){if(method.empty()||method.front()=='$'||method.find_first_of(".\0",0,2)!=std::string::npos)throw std::invalid_argument("invalid exposed entity method");auto route=type+'.'+method;auto weak=std::weak_ptr(impl_);
        impl_->front.bind_rpc(route,{true,false,false},[weak,type,route](const CallIdentity& who,std::string input,Reply reply){if(auto self=weak.lock())self->routed(who,type,route,std::move(input),false,std::move(reply));else reply({Status::disconnected,{}});});impl_->bound.push_back(route);}
}
void ClientGateway::bind_account(std::string route,std::string type){auto weak=std::weak_ptr(impl_);impl_->front.bind_rpc(route,{true,false,false},[weak,type,route](const CallIdentity& who,std::string input,Reply reply){if(auto self=weak.lock())self->routed(who,type,route,std::move(input),true,std::move(reply));else reply({Status::disconnected,{}});});impl_->bound.push_back(std::move(route));}
void ClientGateway::poll(std::size_t budget){auto self=impl_;if(self->stopped)return;self->storage.poll(budget);auto now=Clock::now();std::vector<std::uint64_t> dead;
    for(const auto& [peer,s]:self->sessions)if(!self->authenticated(peer,s.subject))dead.push_back(peer);
    for(auto peer:dead){self->front.close(peer);auto subs=self->subscriptions;for(auto& [id,s]:subs){(void)id;if(s->client==peer)self->remove(s,Status::disconnected,false);}auto it=self->sessions.find(peer);if(it!=self->sessions.end()){for(auto& [index,l]:it->second.links){(void)index;self->back.close(l->peer);self->links.erase(l->peer);}self->sessions.erase(it);}}
    std::vector<std::uint64_t> lost;
    for(const auto& [peer,l]:self->links)if(!l->opening&&!self->back.connected(peer))lost.push_back(peer);
    for(auto peer:lost){auto l=self->links.at(peer);auto it=self->sessions.find(l->client);if(it!=self->sessions.end()){auto found=it->second.links.find(l->index);if(found!=it->second.links.end()&&found->second==l)it->second.links.erase(found);}self->links.erase(peer);}
    auto req=self->requests;for(auto& [id,r]:req){(void)id;if(now>=r->deadline)self->finish(r,{Status::timeout,{}});else if(!self->authenticated(r->client,r->subject))self->finish(r,{Status::disconnected,{}});}
    auto count=self->schedule.size();std::size_t work=0;
    while(count-- && work++<budget && !self->schedule.empty()){
        auto id=self->schedule.front();self->schedule.pop_front();auto it=self->subscriptions.find(id);if(it==self->subscriptions.end())continue;auto s=it->second;self->schedule.push_back(id);
        if(s->link&&s->link->ready&&!self->back.connected(s->link->peer))self->migrating(s,true);
        if(s->migrating&&now>=s->deadline)self->remove(s,Status::timeout);else if(!s->busy&&now>=s->next)self->refresh(s);
    }
}
GatewayStats ClientGateway::stats()const{auto r=impl_->counters;r.clients=impl_->sessions.size();r.links=impl_->links.size();r.subscriptions=impl_->subscriptions.size();r.pending=impl_->requests.size();for(const auto& [id,s]:impl_->subscriptions){(void)id;if(s->migrating)++r.migrating;}return r;}
void ClientGateway::drain(){impl_->draining=true;}
void ClientGateway::stop(){impl_->stop();}
}

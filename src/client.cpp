#include "fsnet/client.hpp"
#include "fsnet/client_script_host.hpp"
#include "fsnet/entity_message.hpp"
#include <unordered_map>
#include <utility>
namespace fsnet {
struct Client::Impl : std::enable_shared_from_this<Impl> {
    using Clock=std::chrono::steady_clock;
    struct Binding { EntityKey key; bool automatic; std::optional<bool> owned; bool controlled; std::uint64_t revision; };
    std::shared_ptr<Node> node;
    std::shared_ptr<ClientScriptHost> host;
    std::unordered_map<std::uint64_t,Binding> bindings;
    Events events;
    ClientState state=ClientState::disconnected;
    std::uint64_t generation=0,peer=0;
    std::chrono::milliseconds connect_timeout,request_timeout;
    Clock::time_point deadline{};
    bool polling=false,applying=false,shutdown=false,disconnect_pending=false;
    Status disconnect_status=Status::ok;
    std::optional<ClientEndpoint> next_connect;
    std::string error;
    explicit Impl(ClientOptions options)
      : node(std::make_shared<Node>(std::move(options.token),options.transport,std::move(options.tls))),
        connect_timeout(options.connect_timeout),request_timeout(options.request_timeout) {
        for(auto timeout:{connect_timeout,request_timeout})if(timeout.count()<=0 || timeout>std::chrono::seconds(30))throw std::invalid_argument("invalid client timeout");
    }
    bool active() const { return !shutdown && !disconnect_pending && state!=ClientState::disconnected && host && peer; }
    template<class F> void invoke(F&& fn) {
        if(shutdown)return;
        try { fn(); }
        catch(const std::exception& e) { error=std::string(e.what()).substr(0,4096);request_disconnect(Status::internal); }
        catch(...) { error="client callback threw";request_disconnect(Status::internal); }
    }
    void emit(ClientEvent event) { if(events && !shutdown){auto callback=events;invoke([&]{callback(event);});} }
    void emit_connection(ClientEvent::Kind kind,Status status=Status::ok) {
        ClientEvent event{};event.kind=kind;event.generation=generation;event.status=status;emit(std::move(event));
    }
    void emit_binding(ClientEvent::Kind kind,std::uint64_t id,const Binding& b,Status status=Status::ok) {
        emit({kind,generation,status,b.key,{generation,id},b.automatic,b.owned,b.controlled});
    }
    void ready() {
        if(active() && state==ClientState::connecting && node->connected(peer)) {
            state=ClientState::connected;emit_connection(ClientEvent::Kind::connected);
        }
    }
    void request_disconnect(Status status) { disconnect_pending=true;disconnect_status=status; }
    void changed(std::uint64_t id,Status status,const AttributeReplica* replica) {
        ready();if(!active())return;
        auto it=bindings.find(id);
        if(status!=Status::ok || !replica) {
            if(it!=bindings.end()){auto old=std::move(it->second);bindings.erase(it);emit_binding(ClientEvent::Kind::leave,id,old,status);}
            return;
        }
        if(it==bindings.end()) {
            auto info=host->projection_info(id);if(!info)return;
            Binding b{{info->type,info->id},info->automatic,info->automatic?std::optional(info->owned):std::nullopt,info->controlled,replica->revision()};
            bindings.emplace(id,b);emit_binding(ClientEvent::Kind::enter,id,b);
        } else if(it->second.revision!=replica->revision()) {
            it->second.revision=replica->revision();auto b=it->second;emit_binding(ClientEvent::Kind::changed,id,b);
        }
    }
    void presence(const EntityPresence& event) {
        ready();if(!active())return;
        auto it=bindings.find(event.subscription);
        if(event.kind==EntityPresence::Kind::leave) {
            if(it!=bindings.end()){auto old=std::move(it->second);bindings.erase(it);emit_binding(ClientEvent::Kind::leave,event.subscription,old,Status::disconnected);}
        } else if(it!=bindings.end() && it->second.controlled!=event.controlled) {
            it->second.controlled=event.controlled;auto b=it->second;emit_binding(ClientEvent::Kind::control,event.subscription,b);
        }
    }
    void start(ClientEndpoint endpoint) {
        if(generation==UINT64_MAX)throw std::overflow_error("client generation exhausted");
        auto new_peer=node->connect(endpoint.address,endpoint.port,endpoint.server_name);
        std::shared_ptr<ClientScriptHost> new_host;
        try { new_host=std::make_shared<ClientScriptHost>(node,new_peer,false); }
        catch(...) { node->close(new_peer);throw; }
        ++generation;peer=new_peer;host=new_host;state=ClientState::connecting;
        deadline=Clock::now()+connect_timeout;error.clear();
        auto weak=weak_from_this();auto epoch=generation;
        node->identify([weak,epoch,new_peer](std::uint64_t incoming,const std::string& certificate)->std::optional<CallIdentity> {
            auto self=weak.lock();
            if(!self || !self->active() || self->generation!=epoch || incoming!=new_peer)return {};
            return CallIdentity{Caller::server,certificate.empty()?"configured-server":certificate,incoming};
        });
        host->observe([weak,epoch](auto id,auto status,auto* replica) {
            if(auto self=weak.lock();self && self->generation==epoch)self->invoke([&]{self->changed(id,status,replica);});
        },[weak,epoch](const auto& event) {
            if(auto self=weak.lock();self && self->generation==epoch)self->invoke([&]{self->presence(event);});
        });
        emit_connection(ClientEvent::Kind::connecting);
    }
    void cleanup() {
        disconnect_pending=false;
        auto old_host=std::exchange(host,{});auto old_peer=std::exchange(peer,0);
        const bool was_active=state!=ClientState::disconnected;
        state=ClientState::disconnected;
        auto old_bindings=std::move(bindings);bindings.clear();
        if(old_host){old_host->observe({},{});old_host->stop();}
        if(old_peer)node->close(old_peer);
        for(const auto& [id,b]:old_bindings)emit_binding(ClientEvent::Kind::leave,id,b,Status::disconnected);
        if(was_active)emit_connection(ClientEvent::Kind::disconnected,disconnect_status);
    }
    void apply_actions() {
        if(polling || applying)return;
        applying=true;
        try {
            if(disconnect_pending)cleanup();
            if(shutdown){next_connect.reset();node->stop();}
            else if(next_connect && state==ClientState::disconnected) {
                auto endpoint=std::move(*next_connect);next_connect.reset();start(std::move(endpoint));
            }
            if(shutdown && host){cleanup();node->stop();}
            applying=false;
        } catch(...) { applying=false;throw; }
    }
    std::chrono::milliseconds timeout(std::chrono::milliseconds value) const { return value.count()==0?request_timeout:value; }
    Reply guarded(Reply reply) {
        if(!reply)throw std::invalid_argument("missing reply callback");
        return [weak=weak_from_this(),epoch=generation,reply=std::move(reply)](Result result) mutable {
            auto self=weak.lock();if(!self || self->shutdown)return;
            self->ready();
            if(self->generation!=epoch || !self->active())result={Status::disconnected,{}};
            self->invoke([&]{reply(std::move(result));});
        };
    }
};
Client::Client(ClientOptions options):impl_(std::make_shared<Impl>(std::move(options))) {}
Client::~Client(){auto self=impl_;self->shutdown=true;self->events={};self->request_disconnect(Status::ok);self->apply_actions();}
void Client::on_event(Events fn){impl_->events=std::move(fn);}
void Client::connect(ClientEndpoint endpoint){
    auto self=impl_;
    if(endpoint.address.empty() || !endpoint.port)throw std::invalid_argument("invalid client endpoint");
    if(self->shutdown || (self->state!=ClientState::disconnected && !self->disconnect_pending))throw std::logic_error("client already connecting or connected");
    if(self->next_connect)throw std::logic_error("client reconnect already queued");
    self->next_connect=std::move(endpoint);self->apply_actions();
}
void Client::disconnect(){auto self=impl_;self->request_disconnect(Status::ok);self->apply_actions();}
void Client::poll(){
    auto self=impl_;if(self->polling)throw std::logic_error("recursive client poll");
    self->apply_actions();if(self->shutdown)return;
    self->polling=true;
    // Pin the session adapter through all Node callbacks. Reconnect/teardown
    // requested by callbacks is deferred until the networking poll returns.
    auto pinned=self->host;
    try {
        self->node->poll();
        if(self->active())self->host->poll();
        self->ready();
        if(self->active() && !self->node->connected(self->peer)) {
            if(!self->node->connecting(self->peer))self->request_disconnect(Status::disconnected);
            else if(Impl::Clock::now()>=self->deadline)self->request_disconnect(Status::timeout);
        }
        self->polling=false;
    } catch(...) { self->polling=false;self->request_disconnect(Status::internal);self->apply_actions();throw; }
    self->apply_actions();
}
ClientState Client::state() const{return impl_->state;}
ClientStats Client::stats() const{return {impl_->state,impl_->generation,impl_->bindings.size(),impl_->node->stats()};}
std::string Client::last_error() const{return impl_->error;}
void Client::request(std::string_view route,std::string_view payload,Reply reply,std::chrono::milliseconds timeout){
    auto self=impl_;auto guarded=self->guarded(std::move(reply));
    if(!self->active())return guarded({Status::disconnected,{}});
    self->node->call(self->peer,route,payload,self->timeout(timeout),std::move(guarded));
}
void Client::call(const EntityKey& key,std::string_view method,std::string_view payload,Reply reply,ClientRequestOptions options){
    // Only envelope construction is caught; do not invoke a callback twice if
    // user code itself throws. guarded() contains application callback failures.
    std::string route,message;
    try {
        route=entity_call_route;message=entity_call_message(EntityId(key.id),method,payload,options.request_id,key.type);
    }
    catch(const std::exception&){auto self=impl_;return self->guarded(std::move(reply))({Status::rejected,{}});}
    request(route,message,std::move(reply),options.timeout);
}
void Client::legacy_call(const EntityKey& key,std::string_view method,std::string_view payload,Reply reply,ClientRequestOptions options) {
    std::string route,message;
    try{route=entity_route(key.type,method);message=entity_message(key.id,payload,options.request_id);}
    catch(...){return impl_->guarded(std::move(reply))({Status::rejected,{}});}
    request(route,message,std::move(reply),options.timeout);
}
void Client::call(EntityId id,std::string_view method,std::string_view payload,Reply reply,ClientRequestOptions options) {
    std::string message;
    try{message=entity_call_message(id,method,payload,options.request_id);}
    catch(...){return impl_->guarded(std::move(reply))({Status::rejected,{}});}
    request(entity_call_route,message,std::move(reply),options.timeout);
}
std::optional<ClientEntity> Client::entity(EntityId id) const {
    auto text=id.str();for(const auto& [subscription,binding]:impl_->bindings)if(binding.key.id==text)return entity(ClientEntityHandle{impl_->generation,subscription});
    return {};
}
void Client::watch(EntityKey key,Ready reply,std::chrono::milliseconds timeout){
    if(!reply)throw std::invalid_argument("missing watch callback");
    if(!valid_entity_id(key.id)){auto self=impl_;return self->invoke([&]{reply(Status::rejected,{});});}
    legacy_watch(std::move(key),std::move(reply),timeout);
}
void Client::legacy_watch(EntityKey key,Ready reply,std::chrono::milliseconds timeout){
    if(!reply)throw std::invalid_argument("missing watch callback");
    auto self=impl_;if(!self->active())return self->invoke([&]{reply(Status::disconnected,{});});
    const auto epoch=self->generation;
    auto pinned=self->host;
    pinned->watch_attributes(std::move(key.type),std::move(key.id),[weak=std::weak_ptr(self),epoch,reply=std::move(reply)](Status status,std::uint64_t id) mutable {
        auto self=weak.lock();if(!self || self->shutdown)return;
        self->ready();
        if(self->generation!=epoch || !self->active())status=Status::disconnected;
        // AttributeClient invokes Ready before Changed, so install metadata now.
        if(status==Status::ok && !self->bindings.contains(id)) {
            auto info=self->host->projection_info(id);auto image=self->host->attribute_snapshot(id);
            if(!info || !image)status=Status::disconnected;
            else {Impl::Binding b{{info->type,info->id},info->automatic,info->automatic?std::optional(info->owned):std::nullopt,info->controlled,image->revision};self->bindings.emplace(id,b);self->emit_binding(ClientEvent::Kind::enter,id,b);}
        }
        if(status==Status::ok && (!self->active() || !self->bindings.contains(id)))status=Status::disconnected;
        self->invoke([&]{reply(status,status==Status::ok?ClientEntityHandle{epoch,id}:ClientEntityHandle{});});
    },self->timeout(timeout));
}
void Client::write(ClientEntityHandle handle,EntityAttributes::Values values,Reply reply,AttributeWriteOptions options,std::chrono::milliseconds timeout){
    auto self=impl_;auto guarded=self->guarded(std::move(reply));
    if(!self->active())return guarded({Status::disconnected,{}});
    if(handle.generation!=self->generation || !self->bindings.contains(handle.subscription))return guarded({Status::rejected,{}});
    auto pinned=self->host;
    pinned->write_attributes(handle.subscription,std::move(values),std::move(guarded),std::move(options),self->timeout(timeout));
}
void Client::unwatch(ClientEntityHandle handle){auto self=impl_;auto pinned=self->host;if(handle.generation==self->generation && pinned)pinned->unwatch_attributes(handle.subscription);}
std::optional<ClientEntity> Client::entity(ClientEntityHandle handle) const {
    auto self=impl_;if(handle.generation!=self->generation || !self->active())return {};
    auto it=self->bindings.find(handle.subscription);if(it==self->bindings.end())return {};
    auto image=self->host->attribute_snapshot(handle.subscription);if(!image)return {};
    const auto& b=it->second;return ClientEntity{b.key,handle,std::move(image),b.automatic,b.owned,b.controlled};
}
std::optional<ClientEntity> Client::entity(const EntityKey& key) const {
    std::optional<ClientEntityHandle> manual;
    for(const auto& [id,b]:impl_->bindings)if(b.key==key){if(b.automatic)return entity({impl_->generation,id});manual=ClientEntityHandle{impl_->generation,id};}
    return manual?entity(*manual):std::nullopt;
}
std::optional<ClientEntity> Client::controlled() const {
    for(const auto& [id,b]:impl_->bindings)if(b.controlled)return entity({impl_->generation,id});
    return {};
}
std::vector<ClientEntity> Client::entities() const {
    std::vector<ClientEntity> out;out.reserve(impl_->bindings.size());
    for(const auto& [id,b]:impl_->bindings){(void)b;auto view=entity({impl_->generation,id});if(view)out.push_back(std::move(*view));}
    return out;
}
std::shared_ptr<ScriptHost> Client::script_host(){
    auto self=impl_;if(self->polling || !self->active())throw std::logic_error("acquire a script host outside poll on an active connection");
    auto pinned=self->host;pinned->enable_entity_events();return pinned;
}
}

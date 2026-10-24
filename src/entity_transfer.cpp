#include "fsnet/entity_transfer.hpp"
#include "fsnet/replication_wire.hpp"
#include <algorithm>
#include <unordered_map>
#include <utility>

namespace fsnet {
namespace {
constexpr const char* probe_route="system.entity_transfer.probe";
constexpr const char* accept_route="system.entity_transfer.accept";
std::pair<std::string,std::string> identity(std::string_view& input) {
    if(input.size()>1024) throw std::invalid_argument("transfer message too large");
    return decode_entity_key(input);
}
}
struct EntityTransfer::Impl:std::enable_shared_from_this<Impl> {
    struct Operation {
        std::string type,id,target,key;
        std::size_t endpoint=0;
        Reply reply;
        bool done=false;
    };
    Node& node;
    std::shared_ptr<EntityScriptHost> host;
    AsyncDatabase storage;
    std::vector<EntityEndpoint> endpoints;
    Trust trust;
    EntityTransferLimits limits;
    std::unordered_map<std::string,std::shared_ptr<Operation>> operations;
    std::vector<std::uint64_t> peers;
    bool stopped=false;
    Impl(Node& n,std::shared_ptr<EntityScriptHost> h,std::shared_ptr<Database> db,
         std::vector<EntityEndpoint> e,Trust t,EntityTransferLimits l)
        :node(n),host(std::move(h)),storage(std::move(db)),endpoints(std::move(e)),trust(std::move(t)),limits(l) {
        validate_entity_endpoints(endpoints);
        if(!host || !trust || !limits.requests || limits.requests>1024 || limits.timeout.count()<=0 || limits.timeout>std::chrono::minutes(5))
            throw std::invalid_argument("invalid entity transfer settings");
        peers.resize(endpoints.size());
    }
    void finish(const std::shared_ptr<Operation>& op,Result result) {
        if(std::exchange(op->done,true)) return;
        operations.erase(op->key);
        auto reply=std::move(op->reply); reply(std::move(result));
    }
    void send(const std::shared_ptr<Operation>& op,const char* route,std::string data,Reply reply) {
        try {
            auto& peer=peers[op->endpoint]; auto& e=endpoints[op->endpoint];
            if(!peer || (!node.connected(peer) && !node.connecting(peer))) peer=node.connect(e.address,e.port,e.tls_peer_name);
            node.call(peer,route,data,limits.timeout,std::move(reply));
        } catch(...) { finish(op,{Status::disconnected,{}}); }
    }
    void confirm(const std::shared_ptr<Operation>& op,std::uint64_t ticket) {
        auto data=encode_entity_key(op->type,op->id); entity_record_detail::integer(data,ticket);
        send(op,accept_route,std::move(data),[weak=weak_from_this(),op](Result result) {
            auto self=weak.lock(); if(!self || self->stopped || op->done) return;
            result.data.clear(); self->finish(op,std::move(result));
        });
    }
    void start(std::string type,std::string id,std::string target,Reply reply) {
        if(!reply) throw std::invalid_argument("transfer callback required");
        if(stopped) return reply({Status::disconnected,{}});
        try { (void)encode_entity_key(type,id); } catch(...) { return reply({Status::rejected,{}}); }
        if(!valid_entity_node(target) || entity_node_matches(host->owner_id(),target)) return reply({Status::rejected,{}});
        auto found=std::find_if(endpoints.begin(),endpoints.end(),[&](const auto& e) {
            return e.node==target && std::find(e.types.begin(),e.types.end(),type)!=e.types.end();
        });
        if(found==endpoints.end()) return reply({Status::not_found,{}});
        auto key=type+'\0'+id;
        if(operations.size()>=limits.requests || operations.contains(key)) return reply({Status::overloaded,{}});
        auto op=std::make_shared<Operation>(); op->type=std::move(type); op->id=std::move(id); op->target=std::move(target);
        op->key=std::move(key); op->endpoint=static_cast<std::size_t>(found-endpoints.begin()); op->reply=std::move(reply);
        operations.emplace(op->key,op);
        send(op,probe_route,encode_entity_key(op->type,op->id),[weak=weak_from_this(),op](Result result) {
            auto self=weak.lock(); if(!self || self->stopped || op->done) return;
            if(result.status!=Status::ok) return self->finish(op,std::move(result));
            if(result.data!=op->target) return self->finish(op,{Status::rejected,{}});
            self->release(op);
        });
    }
    void release(const std::shared_ptr<Operation>& op) {
        if(auto handle=host->handle(op->type,op->id); handle && host->active(op->type,op->id)) {
            host->handoff(*handle,op->target,[weak=weak_from_this(),op](Result result) {
                auto self=weak.lock(); if(!self || self->stopped || op->done) return;
                if(result.status!=Status::ok) return self->finish(op,std::move(result));
                try { std::string_view input(result.data); auto ticket=entity_record_detail::integer(input); if(!input.empty() || !ticket) throw std::runtime_error("invalid transfer ID"); self->confirm(op,ticket); }
                catch(...) { self->finish(op,{Status::internal,{}}); }
            });
            return;
        }
        // Resume after lost acknowledgement, coordinator restart, or source crash
        // following the durable release. Never rewrite the destination here.
        auto record=std::make_shared<std::optional<EntityLocation>>();
        if(!storage.submit(op->key.size(),[record,op](Database& db) {
            *record=entity_location(db,op->type,op->id);
        },[weak=weak_from_this(),record,op](std::exception_ptr error) {
            auto self=weak.lock(); if(!self || self->stopped || op->done) return;
            if(error) return self->finish(op,{Status::internal,{}});
            if(!*record) return self->finish(op,{Status::not_found,{}});
            if((*record)->placement!=op->target || !(*record)->transfer_id) return self->finish(op,{Status::rejected,{}});
            self->confirm(op,(*record)->transfer_id);
        })) finish(op,{Status::overloaded,{}});
    }
};
EntityTransfer::EntityTransfer(Node& node,std::shared_ptr<EntityScriptHost> host,std::shared_ptr<Database> db,
    std::vector<EntityEndpoint> endpoints,Trust trust,EntityTransferLimits limits)
    :impl_(std::make_shared<Impl>(node,std::move(host),std::move(db),std::move(endpoints),std::move(trust),limits)) {
    if(node.has_route(probe_route)||node.has_route(accept_route)) throw std::logic_error("transfer routes already bound");
    auto weak=std::weak_ptr(impl_);
    try {
        node.bind_rpc(probe_route,{false,true,false},[weak](const CallIdentity& who,std::string data,Reply reply) {
            auto self=weak.lock(); if(!self || self->stopped) return reply({Status::disconnected,{}});
            try {
                if(!self->trust(who)) return reply({Status::rejected,{}});
                std::string_view input(data); auto [type,id]=identity(input);
                if(!input.empty()) return reply({Status::rejected,{}});
                if(!self->host->accepts_handoff(type,id)) return reply({Status::overloaded,{}});
                auto owner=self->host->owner_id(); auto split=owner.find('/'); owner.resize(split==std::string::npos?owner.size():split);
                reply({Status::ok,std::move(owner)});
            } catch(...) { reply({Status::rejected,{}}); }
        });
        node.bind_rpc(accept_route,{false,true,false},[weak](const CallIdentity& who,std::string data,Reply reply) {
            auto self=weak.lock(); if(!self || self->stopped) return reply({Status::disconnected,{}});
            try {
                if(!self->trust(who)) return reply({Status::rejected,{}});
                std::string_view input(data); auto [type,id]=identity(input); auto ticket=entity_record_detail::integer(input);
                if(!input.empty() || !ticket) return reply({Status::rejected,{}});
                self->host->accept_handoff(std::move(type),std::move(id),ticket,std::move(reply));
            } catch(...) { reply({Status::rejected,{}}); }
        });
        impl_->host->on_transfer([weak](std::string type,std::string id,std::string target,Reply reply) {
            auto self=weak.lock(); if(!self || self->stopped) return reply({Status::disconnected,{}});
            // Route through the same public entry without capturing a facade lifetime.
            self->start(std::move(type),std::move(id),std::move(target),std::move(reply));
        });
    } catch(...) { node.unbind(probe_route); node.unbind(accept_route); throw; }
}
EntityTransfer::~EntityTransfer() { stop(); }
void EntityTransfer::transfer(std::string type,std::string id,std::string target,Reply reply) {
    auto keep=impl_; keep->start(std::move(type),std::move(id),std::move(target),std::move(reply));
}
std::size_t EntityTransfer::poll(std::size_t budget) { auto keep=impl_; return keep->stopped?0:keep->storage.poll(budget); }
std::size_t EntityTransfer::pending() const { return impl_->operations.size(); }
void EntityTransfer::stop() {
    auto keep=impl_; auto& self=*keep; if(std::exchange(self.stopped,true)) return;
    self.host->on_transfer({}); self.node.unbind(probe_route); self.node.unbind(accept_route);
    auto copy=self.operations;
    for(auto& [key,op]:copy) { (void)key; try { self.finish(op,{Status::disconnected,{}}); } catch(...) {} }
    for(auto peer:self.peers) if(peer) self.node.close(peer);
    self.storage.stop();
}
}

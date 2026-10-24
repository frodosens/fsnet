#include "fsnet/entity_delivery.hpp"
#include "fsnet/entity_transfer.hpp"
#include "fsnet/lua_engine.hpp"
#include "fsnet/entity_message.hpp"
#include <filesystem>
#include <algorithm>
#include <atomic>
#include <iostream>
#include <source_location>
#include <thread>
using namespace std::chrono_literals;
#define CHECK(x) do{if(!(x))throw std::runtime_error(std::string(__FILE__)+':'+std::to_string(__LINE__)+" " #x);}while(false)
struct FaultStorage final:fsnet::Storage {
    std::shared_ptr<fsnet::Storage> base;
    std::atomic<bool> fail_source{false},fail_receiver{false};
    std::atomic<unsigned> failed{0};
    explicit FaultStorage(std::shared_ptr<fsnet::Storage> db):base(std::move(db)){}
    std::optional<fsnet::Record> get(std::string_view key) override{return base->get(key);}
    bool erase(std::string_view key,std::uint64_t expected) override{return base->erase(key,expected);}
    bool compare_exchange(std::string_view key,std::uint64_t expected,std::string_view bytes) override{
        bool source=key==fsnet::entity_record_key("Source","alice"),receiver=key==fsnet::entity_record_key("Receiver","bob");
        if((source&&fail_source)||(receiver&&fail_receiver)){
            auto state=fsnet::decode_entity_record(bytes).snapshot.delivery;
            if((source&&!state.outbox.empty()&&fail_source.exchange(false)) || (receiver&&!state.inbox.empty()&&fail_receiver.exchange(false))){++failed;return false;}
        }
        return base->compare_exchange(key,expected,bytes);
    }
};
const char* model=R"(
local Receiver=fsnet.Entity:extend('Receiver',{owner='id'})
Receiver:attr('gold',{type='int64',default=0,sync='owner'})
Receiver:attr('calls',{type='int32',default=0,sync='owner'})
Receiver:rpc('reward',{callers={'server'}})
function Receiver:reward(data) self.gold=self.gold+assert(tonumber(data));self.calls=self.calls+1;return tostring(self.gold) end
Receiver:rpc('read',{callers={'server'}})
function Receiver:read() return tostring(self.gold)..':'..tostring(self.calls) end
Receiver:rpc('deny',{callers={'server'}})
function Receiver:deny(data) self.gold=999999;Receiver:ref('bob'):send('reward','10000');runtime.reject() end
Receiver:rpc('client_only',{callers={'client'}})
function Receiver:client_only() self.gold=999999;return '' end
Receiver:register()
local Source=fsnet.Entity:extend('Source',{owner='id'})
Source:attr('issued',{type='int32',default=0,sync='owner'})
Source:attr('activations',{type='int32',default=0,sync='owner'})
Source:attr('ticket',{type='string',default='',sync='owner'})
function Source:on_activate() self.activations=self.activations+1 end
Source:rpc('issue',{callers={'server'}})
function Source:issue(data)
    self.ticket=Receiver:ref('bob'):send('reward',data);self.issued=self.issued+1;return self.ticket
end
Source:rpc('abort',{callers={'server'}})
function Source:abort(data) Receiver:ref('bob'):send('reward',data);self.issued=999;error('rollback') end
Source:rpc('flood',{callers={'server'}})
function Source:flood() for i=1,100 do Receiver:ref('bob'):send('reward','1') end return '' end
Source:rpc('denied',{callers={'server'}})
function Source:denied(data)
    self.ticket=Receiver:ref('bob'):send('deny',data);return self.ticket
end
Source:rpc('wrong_policy',{callers={'server'}})
function Source:wrong_policy() self.ticket=Receiver:ref('bob'):send('client_only','');return self.ticket end
Source:rpc('missing_method',{callers={'server'}})
function Source:missing_method() self.ticket=Source:reference('Receiver','bob'):send('missing','');return self.ticket end
Source:rpc('status',{callers={'server'}})
function Source:status(ticket)
    local phase,status,response=runtime.delivery_status(ticket~='' and ticket or self.ticket)
    return phase..':'..tostring(status)..':'..response
end
Source:rpc('forget',{callers={'server'}})
function Source:forget(ticket) return tostring(runtime.delivery_forget(ticket~='' and ticket or self.ticket)) end
Source:register()
runtime.rpc('init_source',{callers={'server'},handler=function() Source:spawn_or_restore('alice');return 'ok' end})
runtime.rpc('init_target',{callers={'server'},handler=function() Receiver:spawn_or_restore('bob');return 'ok' end})
)";
std::optional<fsnet::CallIdentity> identity(std::uint64_t,const std::string&){return fsnet::CallIdentity{fsnet::Caller::server,"owner"};}
bool trust(const fsnet::CallIdentity& who){return who.caller==fsnet::Caller::server&&who.subject=="owner";}
void process_server(int argc,char** argv){
    CHECK(argc==6);auto db=fsnet::file_storage(argv[2]);fsnet::Node node("durable");node.identify(identity);
    fsnet::EntityRuntimeLimits limits;limits.lease=600ms;limits.request_timeout=200ms;
    auto host=std::make_shared<fsnet::EntityScriptHost>(std::make_shared<fsnet::NodeScriptHost>(node),db,std::string(argv[3])+"/"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()),limits);
    auto ap=static_cast<std::uint16_t>(std::stoul(argv[4])),bp=static_cast<std::uint16_t>(std::stoul(argv[5]));
    std::vector<fsnet::EntityEndpoint> endpoints={{"a","127.0.0.1",ap,{"Source","Receiver"}},{"b","127.0.0.1",bp,{"Source","Receiver"}}};
    fsnet::EntityDeliveryLimits dl;dl.retry=10ms;dl.refresh=20ms;dl.timeout=150ms;
    fsnet::EntityDelivery delivery(node,host,db,endpoints,trust,dl);fsnet::LuaEngine engine(host);engine.load(model);
    if(std::getenv("FSNET_TEST_DROP_ACK")) {
        node.unbind("system.delivery.receive");node.bind_rpc("system.delivery.receive",{false,true,false},[host](const fsnet::CallIdentity& who,std::string data,fsnet::Reply){
            host->receive_delivery(who,fsnet::decode_delivery(data),[](auto r){if(r.status==fsnet::Status::ok)std::cout<<"committed"<<std::endl;});
        });
    }
    std::cout<<"listening "<<node.listen("127.0.0.1",std::string(argv[3])=="a"?ap:bp)<<std::endl;
    for(;;){node.poll();host->poll();engine.poll();delivery.poll();std::this_thread::sleep_for(1ms);}
}
int main(int argc,char** argv){try{
    if(argc>1){process_server(argc,argv);return 0;}
    auto directory=std::filesystem::temp_directory_path()/("fsnet-durable-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    struct Cleanup{std::filesystem::path path;~Cleanup(){std::filesystem::remove_all(path);}}cleanup{directory};
    auto db=std::make_shared<FaultStorage>(fsnet::file_storage(directory.string()));fsnet::Node a("t"),b("t"),c("t"),admin("t");a.identify(identity);b.identify(identity);c.identify(identity);
    auto ah=std::make_shared<fsnet::EntityScriptHost>(std::make_shared<fsnet::NodeScriptHost>(a),db,"a/boot");
    auto bh=std::make_shared<fsnet::EntityScriptHost>(std::make_shared<fsnet::NodeScriptHost>(b),db,"b/boot");
    auto ch=std::make_shared<fsnet::EntityScriptHost>(std::make_shared<fsnet::NodeScriptHost>(c),db,"c/boot");
    auto ap=a.listen("127.0.0.1",0),bp=b.listen("127.0.0.1",0),cp=c.listen("127.0.0.1",0);
    std::vector<fsnet::EntityEndpoint> endpoints={{"a","127.0.0.1",ap,{"Source","Receiver"}},{"b","127.0.0.1",bp,{"Source","Receiver"}},{"c","127.0.0.1",cp,{"Source","Receiver"}}};
    fsnet::EntityDeliveryLimits dl;dl.retry=2ms;dl.refresh=5ms;dl.timeout=100ms;
    auto ad=std::make_unique<fsnet::EntityDelivery>(a,ah,db,endpoints,trust,dl);
    fsnet::EntityDelivery bd(b,bh,db,endpoints,trust,dl),cd(c,ch,db,endpoints,trust,dl);
    fsnet::EntityTransfer at(a,ah,db,endpoints,trust),bt(b,bh,db,endpoints,trust),ct(c,ch,db,endpoints,trust);
    fsnet::LuaEngine ae(ah),be(bh),ce(ch);ae.load(model);be.load(model);ce.load(model);
    auto peer_a=admin.connect("127.0.0.1",ap),peer_b=admin.connect("127.0.0.1",bp),peer_c=admin.connect("127.0.0.1",cp);
    bool workers=false;
    auto pump=[&]{a.poll();b.poll();c.poll();admin.poll();ah->poll();bh->poll();ch->poll();ae.poll();be.poll();ce.poll();at.poll();bt.poll();ct.poll();if(workers){if(ad)ad->poll();bd.poll();cd.poll();}};
    auto until=[&](auto done,std::source_location where=std::source_location::current()){auto end=std::chrono::steady_clock::now()+6s;while(!done()){pump();if(std::chrono::steady_clock::now()>end)throw std::runtime_error("deadline at "+std::to_string(where.line())+" "+ae.last_error()+" / "+be.last_error()+" / "+ce.last_error());std::this_thread::sleep_for(1ms);}};
    auto wait=[&](auto start){std::optional<fsnet::Result> result;start([&](auto r){result=std::move(r);});until([&]{return result.has_value();});return *result;};
    auto rpc=[&](std::uint64_t peer,std::string route,std::string payload){return wait([&](auto cb){admin.call(peer,route,payload,3s,cb);});};
    auto source=[&](std::string method,std::string payload=""){return rpc(peer_a,"Source."+method,fsnet::entity_message("alice",payload));};
    auto record=[&](std::string type,std::string id){auto r=db->get(fsnet::entity_record_key(type,id));CHECK(r);return fsnet::decode_entity_record(r->value);};
    auto pending=[&]{auto d=record("Source","alice").snapshot.delivery;return std::count_if(d.outbox.begin(),d.outbox.end(),[](const auto& m){return !m.done;});};
    auto gold=[&]{auto state=fsnet::decode_attributes(record("Receiver","bob").snapshot.state);return std::get<std::int64_t>(state.values.at("gold"));};
    CHECK(rpc(peer_a,"init_source","").status==fsnet::Status::ok);CHECK(rpc(peer_b,"init_target","").status==fsnet::Status::ok);
    auto initial=record("Source","alice").snapshot;
    CHECK(source("abort","99").status!=fsnet::Status::ok);CHECK(pending()==0 && gold()==0);
    CHECK(record("Source","alice").snapshot.state==initial.state);
    db->fail_source=true;CHECK(source("issue","77").status!=fsnet::Status::ok);
    CHECK(db->failed==1 && pending()==0 && gold()==0 && record("Source","alice").snapshot.state==initial.state);
    auto before_flood=record("Source","alice").snapshot.state;
    CHECK(source("flood").status!=fsnet::Status::ok);
    CHECK(pending()==0 && gold()==0 && record("Source","alice").snapshot.state==before_flood);
    auto issue=source("issue","7");CHECK(issue.status==fsnet::Status::ok && issue.data.size()==64 && pending()==1 && gold()==0);
    auto ticket=issue.data;auto stored=record("Source","alice").snapshot.delivery;auto original=fsnet::DeliveryEnvelope{"Source","alice",stored.epoch,stored.outbox.front()};
    CHECK(source("status").data=="pending:0:");CHECK(source("forget").data=="false");
    CHECK(wait([&](auto cb){ah->control_entity("Source","alice",true,cb);}).status==fsnet::Status::overloaded);
    // Receiver migrates while its reward remains durably queued.
    CHECK(wait([&](auto cb){bt.transfer("Receiver","bob","c",cb);}).status==fsnet::Status::ok);
    db->fail_receiver=true;workers=true;until([&]{return pending()==0;});CHECK(gold()==7 && db->failed==2);
    CHECK(source("status").data=="succeeded:0:7");CHECK(source("forget").data=="true");
    auto mutated=original;mutated.message.payload="999";
    CHECK(rpc(peer_c,"system.delivery.receive",fsnet::encode_delivery(mutated)).status==fsnet::Status::rejected && gold()==7);
    mutated=original;mutated.message.sequence=2;mutated.message.ticket=fsnet::delivery_ticket(mutated);
    CHECK(rpc(peer_c,"system.delivery.receive",fsnet::encode_delivery(mutated)).status==fsnet::Status::rejected && gold()==7);
    // Watermark survives forgetting results and more than the old 64-receipt window.
    for(int i=0;i<70;++i){CHECK(source("issue","1").status==fsnet::Status::ok);until([&]{return pending()==0;});CHECK(source("forget").data=="true");}
    CHECK(gold()==77);
    auto duplicate=rpc(peer_c,"system.delivery.receive",fsnet::encode_delivery(original));CHECK(duplicate.status==fsnet::Status::ok && gold()==77);
    // Business rejection rolls back target state, records a terminal result, and
    // advances the channel so later commands are not permanently blocked.
    CHECK(source("denied","bad").status==fsnet::Status::ok);until([&]{return pending()==0;});CHECK(gold()==77);
    CHECK(source("status").data.starts_with("failed:5:"));CHECK(record("Receiver","bob").snapshot.delivery.outbox.empty());CHECK(source("forget").data=="true");
    CHECK(source("wrong_policy").status==fsnet::Status::ok);until([&]{return pending()==0;});CHECK(gold()==77);CHECK(source("status").data.starts_with("failed:5:"));CHECK(source("forget").data=="true");
    CHECK(source("missing_method").status==fsnet::Status::ok);until([&]{return pending()==0;});CHECK(source("status").data.starts_with("failed:1:"));CHECK(source("forget").data=="true");
    // Source migrates with its outbox. A replacement worker rediscovers it
    // from persisted registration, without replaying the original source RPC.
    workers=false;CHECK(source("issue","3").status==fsnet::Status::ok);
    CHECK(wait([&](auto cb){at.transfer("Source","alice","b",cb);}).status==fsnet::Status::ok);
    ad.reset();ad=std::make_unique<fsnet::EntityDelivery>(a,ah,db,endpoints,trust,dl);
    workers=true;until([&]{return pending()==0;});CHECK(gold()==80);
    auto moved_source=[&](std::string method,std::string payload=""){return rpc(peer_b,"Source."+method,fsnet::entity_message("alice",payload));};
    CHECK(moved_source("status").data=="succeeded:0:80");CHECK(moved_source("forget").data=="true");
    // A deleted/recreated destination has a different epoch. Old queued work
    // becomes an explicit failure and cannot award its replacement instance.
    workers=false;CHECK(moved_source("issue","1000").status==fsnet::Status::ok);
    CHECK(wait([&](auto cb){ch->control_entity("Receiver","bob",true,cb);}).status==fsnet::Status::ok);
    CHECK(rpc(peer_c,"init_target","").status==fsnet::Status::ok);workers=true;until([&]{return pending()==0;});CHECK(gold()==0);
    CHECK(moved_source("status").data.starts_with("failed:1:"));CHECK(moved_source("forget").data=="true");
    // Codec rejects malformed packets and exact-byte mutation is not authorized
    // by a matching ticket alone: the receiver verifies the durable source entry.
    auto forged=original;forged.message.payload="100000";
    CHECK(rpc(peer_c,"system.delivery.receive",fsnet::encode_delivery(forged)).status==fsnet::Status::ok);CHECK(gold()==0); // old destination epoch: terminal not_found
    bool malformed=false;try{fsnet::decode_delivery("FSD1");}catch(...){malformed=true;}CHECK(malformed);
    c.identify([](std::uint64_t,const std::string&)->std::optional<fsnet::CallIdentity>{return fsnet::CallIdentity{fsnet::Caller::client,"owner"};});
    CHECK(rpc(peer_c,"system.delivery.receive",fsnet::encode_delivery(original)).status==fsnet::Status::rejected);
    CHECK(rpc(peer_c,"system.delivery.ack","").status==fsnet::Status::rejected);
    c.identify([](std::uint64_t,const std::string&)->std::optional<fsnet::CallIdentity>{return fsnet::CallIdentity{fsnet::Caller::server,"untrusted"};});
    CHECK(rpc(peer_c,"system.delivery.receive",fsnet::encode_delivery(original)).status==fsnet::Status::rejected);
    c.identify(identity);CHECK(gold()==0);
    CHECK(ad->stats().sources==2);
    ad.reset();bd.stop();cd.stop();at.stop();bt.stop();ct.stop();ae.stop();be.stop();ce.stop();ah->stop();bh->stop();ch->stop();
    std::cout<<"PASS durable delivery: atomic outbox/inbox, watermark replay, failure retention, migration, discovery and lifetime isolation\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}

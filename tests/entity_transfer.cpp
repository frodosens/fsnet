#include "fsnet/entity_transfer.hpp"
#include "fsnet/entity_directory.hpp"
#include "fsnet/entity_message.hpp"
#include "fsnet/replication_wire.hpp"
#include "fsnet/lua_engine.hpp"
#include <iostream>
#include <thread>
using namespace std::chrono_literals;
#define CHECK(x) do { if(!(x)) throw std::runtime_error(std::string(__FILE__)+':'+std::to_string(__LINE__)+" " #x); } while(false)
const char* script=R"(
local reject_activation=false
runtime.rpc('reject_activation',{callers={'server'},handler=function(ctx,data) reject_activation=data=='yes'; return 'ok' end})
local Player=fsnet.Entity:extend('Player',{owner='id'})
Player:attr('position',{type='vec3',client='rw',sync='observers'})
Player:attr('activations',{type='int32',default=0,sync='owner'})
Player:attr('fail_release',{type='bool',default=false,sync='owner'})
function Player:on_activate() assert(not reject_activation,'activation failure'); self.activations=self.activations+1 end
function Player:on_deactivate(reason)
    assert(not self.fail_release,'release failure')
    if reason=='transfer' then self.position.y=self.position.y+1 end
end
Player:rpc('set',{callers={'server'}})
function Player:set(data) self.position.x=assert(tonumber(data)); return 'set' end
Player:rpc('fail',{callers={'server'}})
function Player:fail(data) self.fail_release=data=='yes' end
Player:rpc('self_transfer',{callers={'server'}})
function Player:self_transfer() return tostring(Player:transfer(self.id,'b')) end
Player:register()
runtime.rpc('transfer',{callers={'server'},handler=function(ctx,id) return tostring(Player:transfer(id,'b')) end})
)";
int main(){try {
    auto db=fsnet::memory_database(); fsnet::Node a("t"),b("t"),c("t"),admin("t"),intruder("t");
    auto trusted=[](std::uint64_t,const std::string&)->std::optional<fsnet::CallIdentity>{return fsnet::CallIdentity{fsnet::Caller::server,"owner"};};
    a.identify(trusted); b.identify(trusted); c.identify(trusted);
    std::int64_t clock=10000;
    fsnet::EntityRuntimeLimits limits; limits.lease=1000ms; limits.request_timeout=500ms;
    auto ah=std::make_shared<fsnet::EntityScriptHost>(std::make_shared<fsnet::NodeScriptHost>(a),db,"a/boot",limits,fsnet::StorageLimits{},[&]{return clock;});
    auto bh=std::make_shared<fsnet::EntityScriptHost>(std::make_shared<fsnet::NodeScriptHost>(b),db,"b/boot",limits,fsnet::StorageLimits{},[&]{return clock;});
    auto ch=std::make_shared<fsnet::EntityScriptHost>(std::make_shared<fsnet::NodeScriptHost>(c),db,"c/boot",limits,fsnet::StorageLimits{},[&]{return clock;});
    fsnet::LuaEngine ae(ah),be(bh),ce(ch); ae.load(script); be.load(script); ce.load(script);
    auto ap=a.listen("127.0.0.1",0),bp=b.listen("127.0.0.1",0),cp=c.listen("127.0.0.1",0);
    std::vector<fsnet::EntityEndpoint> endpoints={{"a","127.0.0.1",ap,{"Player"}},{"b","127.0.0.1",bp,{"Player"}},{"c","127.0.0.1",cp,{"Player"}},{"alias","127.0.0.1",bp,{"Player"}}};
    auto trust=[](const fsnet::CallIdentity& who){return who.subject=="owner";};
    fsnet::EntityTransferLimits transfer_limits;transfer_limits.timeout=200ms;
    fsnet::EntityTransfer at(a,ah,db,endpoints,trust,transfer_limits),bt(b,bh,db,endpoints,trust),ct(c,ch,db,endpoints,trust);
    auto pump=[&]{a.poll();b.poll();c.poll();admin.poll();intruder.poll();ah->poll();bh->poll();ch->poll();ae.poll();be.poll();ce.poll();at.poll();bt.poll();ct.poll();};
    auto until=[&](auto done){auto deadline=std::chrono::steady_clock::now()+5s;while(!done()){pump();CHECK(std::chrono::steady_clock::now()<deadline);std::this_thread::sleep_for(1ms);}};
    auto wait=[&](auto start){std::optional<fsnet::Result> r;start([&](auto v){r=std::move(v);});until([&]{return r.has_value();});return *r;};
    auto read=[&](auto h,const std::string& id){return wait([&](auto cb){h->read_attributes("Player",id,cb);});};
    auto record=[&](const std::string& id){auto r=db->get(fsnet::entity_record_key("Player",id));CHECK(r);return fsnet::decode_entity_record(r->value);};
    auto call=[&](const std::string& route,const std::string& data){auto peer=admin.connect("127.0.0.1",ap);return wait([&](auto cb){admin.call(peer,route,data,2s,cb);});};
    CHECK(read(ah,"alice").status==fsnet::Status::ok);
    CHECK(call("Player.set",fsnet::entity_message("alice","7","move-once")).status==fsnet::Status::ok);
    auto stale=*ah->handle("Player","alice");
    // Preflight rejects invalid/missing destinations without releasing source.
    CHECK(wait([&](auto cb){at.transfer("Player","alice","missing",cb);}).status==fsnet::Status::not_found);
    CHECK(wait([&](auto cb){at.transfer("Player","alice","a",cb);}).status==fsnet::Status::rejected);
    CHECK(wait([&](auto cb){at.transfer("Player","alice","alias",cb);}).status==fsnet::Status::rejected);
    CHECK(ah->active("Player","alice") && record("alice").placement.empty());
    std::optional<fsnet::Result> queued;
    ah->read_attributes("Player","alice",[&](auto r){queued=std::move(r);});
    CHECK(wait([&](auto cb){ah->handoff(stale,"b",cb);}).status==fsnet::Status::rejected);
    until([&]{return queued.has_value();});CHECK(queued->status==fsnet::Status::ok);
    // Lua task cannot wait on its own mailbox; state remains unchanged.
    CHECK(call("Player.self_transfer",fsnet::entity_message("alice","")).status!=fsnet::Status::ok);
    CHECK(ah->active("Player","alice"));
    // Failed source lifecycle never publishes placement or releases owner.
    CHECK(call("Player.fail",fsnet::entity_message("alice","yes")).status==fsnet::Status::ok);
    CHECK(wait([&](auto cb){at.transfer("Player","alice","b",cb);}).status!=fsnet::Status::ok);
    CHECK(record("alice").owner=="a/boot" && record("alice").placement.empty());
    CHECK(call("Player.fail",fsnet::entity_message("alice","no")).status==fsnet::Status::ok);
    // Real Lua API waits until target activation and its durable commit.
    auto result=call("transfer","alice"); CHECK(result.status==fsnet::Status::ok && result.data==std::to_string(static_cast<int>(fsnet::Status::ok)));
    CHECK(!ah->active("Player","alice") && bh->active("Player","alice"));
    auto moved=record("alice"); CHECK(moved.owner=="b/boot" && moved.placement=="b" && moved.transfer_id);
    auto attrs=fsnet::decode_attributes(moved.snapshot.state);
    CHECK(std::get<std::int64_t>(attrs.values.at("activations"))==2);
    auto position=std::get<fsnet::AttributeStruct>(attrs.values.at("position"));
    CHECK(std::get<double>(position.at("x"))==7 && std::get<double>(position.at("y"))==1);
    CHECK(wait([&](auto cb){ah->deactivate(stale,cb);}).status==fsnet::Status::rejected);
    CHECK(read(ah,"alice").status==fsnet::Status::overloaded);
    CHECK(read(ch,"alice").status==fsnet::Status::overloaded);
    // Lost ACK retry is idempotent; no extra activation or deactivate.
    CHECK(wait([&](auto cb){at.transfer("Player","alice","b",cb);}).status==fsnet::Status::ok);
    CHECK(record("alice").snapshot.state==moved.snapshot.state);
    // Placement survives lease expiry and cannot fall back to rendezvous.
    clock+=2000;
    CHECK(read(ch,"alice").status==fsnet::Status::overloaded);
    auto location=fsnet::entity_location(*db,"Player","alice"); CHECK(location);
    CHECK(fsnet::select_entity_endpoint(endpoints,"Player","alice",location,clock)==1);
    auto absent=endpoints; absent.erase(absent.begin()+1);
    CHECK(!fsnet::select_entity_endpoint(absent,"Player","alice",location,clock));
    CHECK(read(bh,"alice").status==fsnet::Status::ok);
    CHECK(wait([&](auto cb){bt.transfer("Player","alice","c",cb);}).status==fsnet::Status::ok);
    CHECK(record("alice").placement=="c");
    CHECK(wait([&](auto cb){bh->accept_handoff("Player","alice",moved.transfer_id,cb);}).status==fsnet::Status::rejected);
    CHECK(ch->active("Player","alice"));
    auto current_state=record("alice").snapshot.state;
    CHECK(wait([&](auto cb){ch->accept_handoff("Player","alice",moved.transfer_id,cb);}).status==fsnet::Status::rejected);
    CHECK(ch->active("Player","alice") && record("alice").snapshot.state==current_state);
    // Delete/recreate must never reuse an old handoff ID (storage-version ABA).
    CHECK(wait([&](auto cb){ch->control_entity("Player","alice",true,cb);}).status==fsnet::Status::ok);
    CHECK(read(ah,"alice").status==fsnet::Status::ok);
    CHECK(wait([&](auto cb){at.transfer("Player","alice","b",cb);}).status==fsnet::Status::ok);
    CHECK(record("alice").transfer_id!=moved.transfer_id);
    CHECK(wait([&](auto cb){bh->accept_handoff("Player","alice",moved.transfer_id,cb);}).status==fsnet::Status::rejected);
    CHECK(bh->active("Player","alice"));
    // Simulate source crash after release: durable intent alone resumes handoff.
    CHECK(read(ah,"resume").status==fsnet::Status::ok);
    auto handle=*ah->handle("Player","resume");
    auto release=wait([&](auto cb){ah->handoff(handle,"b",cb);}); CHECK(release.status==fsnet::Status::ok);
    auto pending=record("resume"); CHECK(pending.owner.empty() && pending.placement=="b");
    CHECK(read(ch,"resume").status==fsnet::Status::overloaded);
    CHECK(wait([&](auto cb){at.transfer("Player","resume","c",cb);}).status==fsnet::Status::rejected);
    auto target_admin=admin.connect("127.0.0.1",bp);
    CHECK(wait([&](auto cb){admin.call(target_admin,"reject_activation","yes",1s,cb);}).status==fsnet::Status::ok);
    CHECK(wait([&](auto cb){at.transfer("Player","resume","b",cb);}).status!=fsnet::Status::ok);
    CHECK(record("resume").placement=="b" && !ah->active("Player","resume") && !bh->active("Player","resume"));
    CHECK(wait([&](auto cb){admin.call(target_admin,"reject_activation","no",1s,cb);}).status==fsnet::Status::ok);
    CHECK(wait([&](auto cb){at.transfer("Player","resume","b",cb);}).status==fsnet::Status::ok);
    CHECK(bh->active("Player","resume"));
    // Authenticated transport role is required; payload can't grant it.
    b.identify([](std::uint64_t,const std::string&)->std::optional<fsnet::CallIdentity>{return fsnet::CallIdentity{fsnet::Caller::client,"owner"};});
    auto bad=intruder.connect("127.0.0.1",bp);
    CHECK(wait([&](auto cb){intruder.call(bad,"system.entity_transfer.probe",fsnet::encode_entity_key("Player","resume"),1s,cb);}).status==fsnet::Status::rejected);
    b.identify(trusted);
    // Malformed FER3 is rejected and old record versions remain readable.
    auto encoded=fsnet::encode_entity_record(record("resume")); CHECK(encoded.starts_with("FER3"));
    CHECK(fsnet::decode_entity_record(encoded).placement=="b");
    pending.transfer_id=0; bool invalid=false; try{fsnet::encode_entity_record(pending);}catch(...){invalid=true;}CHECK(invalid);
    fsnet::EntityRecord legacy;CHECK(fsnet::decode_entity_record(fsnet::encode_entity_record(legacy)).initialized);
    legacy.initialized=false;CHECK(!fsnet::decode_entity_record(fsnet::encode_entity_record(legacy)).initialized);
    // Actually drop an acknowledgement after the target committed activation.
    bool drop_ack=true;
    b.unbind("system.entity_transfer.accept");
    b.bind_rpc("system.entity_transfer.accept",{false,true,false},[&](const fsnet::CallIdentity&,std::string bytes,fsnet::Reply reply) {
        std::string_view input(bytes);auto [type,id]=fsnet::decode_entity_key(input);auto fence=fsnet::entity_record_detail::integer(input);CHECK(input.empty());
        bh->accept_handoff(type,id,fence,[&,reply=std::move(reply)](auto r){if(!drop_ack)reply(std::move(r));});
    });
    CHECK(read(ah,"lost-ack").status==fsnet::Status::ok);
    CHECK(wait([&](auto cb){at.transfer("Player","lost-ack","b",cb);}).status==fsnet::Status::timeout);
    CHECK(bh->active("Player","lost-ack") && !ah->active("Player","lost-ack"));
    auto acknowledged=record("lost-ack").snapshot.state;drop_ack=false;
    CHECK(wait([&](auto cb){at.transfer("Player","lost-ack","b",cb);}).status==fsnet::Status::ok);
    CHECK(record("lost-ack").snapshot.state==acknowledged);
    CHECK(read(ah,"quota").status==fsnet::Status::ok);
    std::optional<fsnet::Result> first;
    at.transfer("Player","quota","b",[&](auto r){first=std::move(r);});
    CHECK(wait([&](auto cb){at.transfer("Player","quota","c",cb);}).status==fsnet::Status::overloaded);
    until([&]{return first.has_value();});CHECK(first->status==fsnet::Status::ok);
    // Stop resolves outstanding application callback exactly once.
    int completions=0; at.transfer("Player","resume","b",[&](auto r){CHECK(r.status==fsnet::Status::disconnected);++completions;});
    at.stop();for(int i=0;i<10;++i)pump();CHECK(completions==1 && at.pending()==0);
    bt.stop();ct.stop();ae.stop();be.stop();ce.stop();ah->stop();bh->stop();ch->stop();
    std::cout<<"PASS directed handoff: Lua API, lifecycle rollback, target ACK, durable placement, fencing, retry, third-node isolation and cancellation\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}

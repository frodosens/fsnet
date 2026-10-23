#include "fsnet/attribute_replication.hpp"
#include "fsnet/lua_engine.hpp"
#include "fsnet/entity_message.hpp"
#include <chrono>
#include <iostream>
#include <optional>
#include <thread>
#include <unordered_map>
using namespace std::chrono_literals;
#define CHECK(x) do { if (!(x)) throw std::runtime_error(std::string(__FILE__) + ':' + std::to_string(__LINE__) + " " #x); } while (false)
int main() {
    try {
        fsnet::Node server("test"), alice("test"), bob("test"), logic("test"), stranger("test");
        auto transport=std::make_shared<fsnet::NodeScriptHost>(server);
        auto entities=std::make_shared<fsnet::EntityScriptHost>(transport,fsnet::memory_database(),"replication/boot");
        std::string owner="alice";
        entities->ownership([&](const std::string&,const std::string&) { return owner; });
        fsnet::LuaEngine engine(entities);
        engine.load(R"(
            runtime.entity('player', {attrs={
                hp={type='int32',default=100,sync='observers'},
                gold={type='int64',default=7,sync='owner'},
                secret={type='string',default='secret',client='none',sync='none'},
                temporary={type='int32',default=0,persist=false},
            }, methods={damage={callers={'server'},handler=function(self,ctx,data)
                self.hp=self.hp-tonumber(data); self.gold=self.gold+1; self.temporary=self.temporary+1; return ''
            end}}})
        )");
        auto port=server.listen("127.0.0.1",0);
        std::unordered_map<std::uint64_t,fsnet::CallIdentity> identities;
        server.identify([&](std::uint64_t peer,const std::string&) -> std::optional<fsnet::CallIdentity> {
            auto it=identities.find(peer); return it==identities.end()?std::nullopt:std::optional(it->second);
        });
        auto connect=[&](fsnet::Node& node,fsnet::CallIdentity who) {
            std::uint64_t incoming=0;
            server.authorize([&](std::uint64_t p,const std::string&,const std::string&) { incoming=p; return true; });
            auto peer=node.connect("127.0.0.1",port); bool done=false;
            node.call(peer,"system.health","",1s,[&](fsnet::Result r) { CHECK(r.status==fsnet::Status::ok); done=true; });
            auto deadline=std::chrono::steady_clock::now()+2s;
            while(!done) { server.poll(); node.poll(); CHECK(std::chrono::steady_clock::now()<deadline); std::this_thread::sleep_for(1ms); }
            CHECK(incoming); identities.emplace(incoming,std::move(who)); server.authorize({});
            return std::pair(peer,incoming);
        };
        auto [alice_peer,alice_in]=connect(alice,{fsnet::Caller::client,"alice"});
        auto [bob_peer,bob_in]=connect(bob,{fsnet::Caller::client,"bob"});
        auto [logic_peer,logic_in]=connect(logic,{fsnet::Caller::server,"logic"}); (void)logic_in;
        auto [stranger_peer,stranger_in]=connect(stranger,{fsnet::Caller::client,"stranger"}); (void)stranger_in;
        bool visible=true;
        fsnet::ReplicationLimits limits; limits.per_peer=2; limits.lease=3s;
        fsnet::AttributeReplication replication(server,entities,[&](const fsnet::CallIdentity& who,const std::string&,const std::string&) { return visible && who.subject=="bob"; },limits);
        bool callbacks_valid=true;
        std::size_t alice_changes=0,bob_changes=0; fsnet::Status bob_last=fsnet::Status::ok;
        fsnet::AttributeClient ac(alice,alice_peer,[&](std::uint64_t,fsnet::Status status,const fsnet::AttributeReplica* view) {
            if(status==fsnet::Status::ok) { callbacks_valid &= view && !view->values().contains("secret"); ++alice_changes; }
        });
        fsnet::AttributeClient bc(bob,bob_peer,[&](std::uint64_t,fsnet::Status status,const fsnet::AttributeReplica* view) {
            bob_last=status; if(status==fsnet::Status::ok) { callbacks_valid &= view && view->values().size()==1 && view->values().contains("hp"); ++bob_changes; }
        });
        fsnet::AttributeClient sc(stranger,stranger_peer), lc(logic,logic_peer);
        auto pump=[&](bool renew=true) {
            server.poll(); alice.poll(); bob.poll(); logic.poll(); stranger.poll(); entities->poll(); engine.poll(); replication.poll();
            if(renew) { ac.poll(); bc.poll(); sc.poll(); lc.poll(); }
        };
        auto until=[&](auto predicate) { auto deadline=std::chrono::steady_clock::now()+5s; while(!predicate()) { pump(); CHECK(std::chrono::steady_clock::now()<deadline); std::this_thread::sleep_for(1ms); } };
        auto watch=[&](fsnet::AttributeClient& client,const char* id="42") {
            std::optional<std::pair<fsnet::Status,std::uint64_t>> result;
            client.watch("player",id,[&](fsnet::Status status,std::uint64_t token) { result=std::pair(status,token); });
            until([&] { return result.has_value(); }); return *result;
        };
        auto [as,aid]=watch(ac); auto [bs,bid]=watch(bc); CHECK(as==fsnet::Status::ok && bs==fsnet::Status::ok);
        CHECK(ac.get(aid)->values().size()==3 && bc.get(bid)->values().size()==1);
        CHECK(watch(sc,"unseen").first==fsnet::Status::rejected && !entities->handle("player","unseen"));
        CHECK(watch(lc).first==fsnet::Status::rejected); // server role cannot impersonate a client watch
        auto [extra_status,extra]=watch(ac); CHECK(extra_status==fsnet::Status::ok);
        CHECK(watch(ac).first==fsnet::Status::overloaded); ac.unwatch(extra);
        for(int i=0;i<10;++i) { pump(); std::this_thread::sleep_for(1ms); }
        auto damage=[&](fsnet::Node& caller,std::uint64_t peer,const std::string& request="") {
            std::optional<fsnet::Result> result;
            caller.call(peer,"player.damage",fsnet::entity_message("42","10",request),1s,[&](fsnet::Result r) { result=std::move(r); });
            until([&] { return result.has_value(); }); return result->status;
        };
        CHECK(damage(bob,bob_peer)==fsnet::Status::rejected);
        CHECK(damage(logic,logic_peer,"damage-1")==fsnet::Status::ok);
        until([&] { return std::get<std::int64_t>(ac.get(aid)->values().at("hp"))==90 && std::get<std::int64_t>(bc.get(bid)->values().at("hp"))==90; });
        CHECK(std::get<std::int64_t>(ac.get(aid)->values().at("temporary"))==1);
        auto a_count=alice_changes,b_count=bob_changes;
        CHECK(damage(logic,logic_peer,"damage-1")==fsnet::Status::ok);
        for(int i=0;i<10;++i) pump(); CHECK(alice_changes==a_count && bob_changes==b_count);
        // Another authenticated connection cannot inject a packet into this SDK's server stream.
        fsnet::Node attacker("test"); auto attack_peer=attacker.connect("127.0.0.1",alice.listen("127.0.0.1",0));
        bool attack_ready=false; attacker.call(attack_peer,"system.health","",1s,[&](fsnet::Result r) { CHECK(r.status==fsnet::Status::ok); attack_ready=true; });
        auto deadline=std::chrono::steady_clock::now()+2s;
        while(!attack_ready) { attacker.poll(); pump(); CHECK(std::chrono::steady_clock::now()<deadline); std::this_thread::sleep_for(1ms); }
        CHECK(attacker.notify(attack_peer,"system.attributes.update","malformed"));
        for(int i=0;i<10;++i) { attacker.poll(); pump(); std::this_thread::sleep_for(1ms); }
        CHECK(ac.get(aid) && alice.connected(alice_peer));
        visible=false; until([&] { return !bc.get(bid); }); CHECK(bob_last==fsnet::Status::rejected);
        visible=true; auto again=watch(bc); CHECK(again.first==fsnet::Status::ok); bid=again.second;
        identities.at(bob_in).subject="mallory"; until([&] { return !bc.get(bid); });
        identities.at(bob_in).subject="bob";
        // No calls to the client manager: server lease expiration revokes the projection.
        auto expiry=std::chrono::steady_clock::now()+3500ms;
        while(ac.get(aid) && std::chrono::steady_clock::now()<expiry) { pump(false); std::this_thread::sleep_for(1ms); }
        CHECK(!ac.get(aid));
        auto fresh=watch(ac); CHECK(fresh.first==fsnet::Status::ok); aid=fresh.second;
        CHECK(std::get<std::int64_t>(ac.get(aid)->values().at("hp"))==90);
        auto handle=entities->handle("player","42"); CHECK(handle);
        bool released=false; entities->deactivate(*handle,[&](fsnet::Result r) { CHECK(r.status==fsnet::Status::ok); released=true; });
        until([&] { return released && !ac.get(aid); });
        auto reactivated=watch(ac); CHECK(reactivated.first==fsnet::Status::ok);
        CHECK(std::get<std::int64_t>(ac.get(reactivated.second)->values().at("temporary"))==0);
        owner="bob"; until([&] { return !ac.get(reactivated.second); });
        CHECK(callbacks_valid);
        (void)alice_in;
        std::cout << "PASS property replication: snapshots/deltas, ACLs, receipts, spoof rejection, quotas, leases, identity/AOI revocation and transient reactivation\n";
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}

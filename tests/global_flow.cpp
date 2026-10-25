#include "fsnet/client.hpp"
#include "fsnet/client_gateway.hpp"
#include "fsnet/entity_transfer.hpp"
#include "fsnet/entity_delivery.hpp"
#include "fsnet/entity_timers.hpp"
#include "fsnet/attribute_replication.hpp"
#include "fsnet/lua_engine.hpp"
#include "fsnet/entity_message.hpp"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <source_location>
#include <thread>
using namespace fsnet;
using namespace std::chrono_literals;
#define CHECK(c) do{if(!(c))throw std::runtime_error(std::string(__FILE__)+":"+std::to_string(__LINE__)+" " #c);}while(false)
std::string read(const std::string& path){std::ifstream in(path);CHECK(in);return {std::istreambuf_iterator<char>(in),{}};}
int main(int argc,char** argv){try{
    CHECK(argc==2 || (argc==3 && std::string(argv[2])=="--benchmark"));auto source=read(std::string(argv[1])+"/examples/entities/shared.lua")+read(std::string(argv[1])+"/examples/entities/server.lua");
    // Select the source-node login deterministically so this fixture always
    // exercises handoff. The example itself chooses randomly.
    auto random=source.find("math.random(#self.logins)");CHECK(random!=std::string::npos);source.replace(random,25,"1");
    auto manager_registration=source.find("Manager:register()");CHECK(manager_registration!=std::string::npos);
    source.insert(manager_registration,"Manager:rpc('unlisted',{callers={'client'}})\nfunction Manager:unlisted() return 'private-to-this-network' end\n");
    auto player_define=source.find("Player:define()");CHECK(player_define!=std::string::npos);
    source.insert(player_define,R"(
        Player:rpc('reward',{callers={'server'}})
        function Player:reward(amount) self.position.x=self.position.x+tonumber(amount) end
        Player:rpc('arm_timer',{callers={'client'},access='owner'})
        function Player:arm_timer() return self:after(250,'reward','10',{key='migration-reward'}) end
    )");
    auto manager_register=source.find("Manager:register()");CHECK(manager_register!=std::string::npos);
    source.insert(manager_register,R"(
        Manager:rpc('reward_player',{callers={'server'}})
        function Manager:reward_player(id) return fsnet.ref(id):send('reward','1') end
        Manager:rpc('spawn_remote',{callers={'client'}})
        function Manager:spawn_remote()
            local player=Player:spawn({name='Remote'},{creation_key='remote-player',target_node='node-b',owner=self.context.connection})
            player:show_to(self.context.connection);player:possess(self.context.connection)
            return player.id
        end
    )");
    Node a("test"),b("test"),front("test"),back("test");auto db=memory_database();std::vector<std::uint64_t> frontend_peers;
    front.authorize([&](std::uint64_t peer,const std::string&,const std::string&){if(std::find(frontend_peers.begin(),frontend_peers.end(),peer)==frontend_peers.end())frontend_peers.push_back(peer);return true;});
    auto ah=std::make_shared<EntityScriptHost>(std::make_shared<NodeScriptHost>(a),db,"node-a/boot");
    auto bh=std::make_shared<EntityScriptHost>(std::make_shared<NodeScriptHost>(b),db,"node-b/boot");
    auto base=[](std::uint64_t,const std::string&)->std::optional<CallIdentity>{return CallIdentity{Caller::server,"loopback-fixture"};};
    auto gateway_trust=[](const CallIdentity& who,const std::string&){return who.caller==Caller::server && who.subject=="loopback-fixture";};
    auto owner_trust=[](const CallIdentity& who){return who.caller==Caller::server && who.subject=="loopback-fixture";};
    GatewayDelegation ad(a,base,gateway_trust,ah),bd(b,base,gateway_trust,bh);
    AttributeReplication ar(a,ah),br(b,bh);
    auto ap=a.listen("127.0.0.1",0),bp=b.listen("127.0.0.1",0);
    std::vector<EntityEndpoint> endpoints={{"node-a","127.0.0.1",ap,{"Manager","LoginEntity","Player"}},{"node-b","127.0.0.1",bp,{"Manager","LoginEntity","Player"}}};
    EntityTransfer at(a,ah,db,endpoints,owner_trust),bt(b,bh,db,endpoints,owner_trust);
    EntityDelivery da(a,ah,db,endpoints,owner_trust),db_delivery(b,bh,db,endpoints,owner_trust);
    EntityTimers ta(ah,db),tb(bh,db);
    EntityRouter ra(a,db,endpoints),rb(b,db,endpoints);
    LuaEngine ae(std::make_shared<RoutedScriptHost>(ah,ra)),be(std::make_shared<RoutedScriptHost>(bh,rb));
    ae.load(source,"global-flow");be.load(source,"global-flow");
    front.identify([](std::uint64_t,const std::string&)->std::optional<CallIdentity>{return CallIdentity{Caller::client,"account-1"};});
    GatewayLimits limits;limits.refresh=5ms;ClientGateway gateway(front,back,db,endpoints,limits);
    gateway.bind_entity("Manager",{"discover","spawn_remote"});
    gateway.bind_entity("LoginEntity",{"login"});
    gateway.bind_entity("Player",{"move","arm_timer","reward"});
    auto port=front.listen("127.0.0.1",0);ClientOptions config;config.token="test";Client client(config),other_client(config);
    auto pump=[&]{a.poll();b.poll();front.poll();back.poll();client.poll();other_client.poll();ah->poll();bh->poll();ae.poll();be.poll();ad.poll();bd.poll();ar.poll();br.poll();ra.poll();rb.poll();at.poll();bt.poll();da.poll();db_delivery.poll();ta.poll();tb.poll();gateway.poll();};
    auto until=[&](auto done,std::source_location where=std::source_location::current()){
        auto end=std::chrono::steady_clock::now()+8s;while(!done()){pump();if(std::chrono::steady_clock::now()>end)throw std::runtime_error("deadline at "+std::to_string(where.line())+": "+ae.last_error()+" / "+be.last_error());std::this_thread::sleep_for(1ms);}
    };
    std::optional<Result> created;ah->create_entity("Manager",{},{"fixture","manager",{},"node-a"},[&](Result value){created=std::move(value);});until([&]{return created.has_value();});
    CHECK(created->status==Status::ok);auto manager=EntityId(created->data);
    // Manager.on_create used the common cross-node protocol to create its pool.
    std::optional<Result> pool;ah->read_attributes("Manager",manager.str(),[&](Result value){pool=std::move(value);});until([&]{return pool.has_value();});CHECK(pool->status==Status::ok);
    auto entries=std::get<AttributeArray>(decode_attributes(pool->data).values.at("logins"));CHECK(entries.size()==2);
    auto remote=EntityId(std::get<std::string>(entries[1]));CHECK(bh->active("LoginEntity",remote.str()));
    auto retry=created;created.reset();ah->create_entity("Manager",{},{"fixture","manager",{},"node-a"},[&](Result value){created=std::move(value);});until([&]{return created.has_value();});CHECK(created->data==retry->data);
    client.connect({"127.0.0.1",port,{}});until([&]{return client.state()==ClientState::connected;});
    auto call=[&](EntityId id,std::string method,std::string payload){std::optional<Result> result;client.call(id,method,payload,[&](Result value){result=std::move(value);});until([&]{return result.has_value();});return *result;};
    CHECK(call(manager,"unlisted","").status==Status::rejected);
    auto discovery=call(manager,"discover","");CHECK(discovery.status==Status::ok);auto login=EntityId(discovery.data);
    auto logged_in=call(login,"login","Alice");CHECK(logged_in.status==Status::ok);auto player=EntityId(logged_in.data);
    until([&]{return client.controlled().has_value() && gateway.stats().migrations>=1 && !gateway.stats().migrating;});
    auto original=client.controlled()->handle;CHECK(client.entity(player)->key.id==player.str());CHECK(bh->active("Player",player.str()));
    CHECK(call(player,"move","3").data=="3.0");
    CHECK(call(player,"reward","1").status==Status::rejected);
    CHECK(call(manager,"reward_player",player.str()).status==Status::rejected);
    std::optional<Result> queued;ra.call(manager,"reward_player",player.str(),3s,[&](Result value){queued=std::move(value);});until([&]{return queued.has_value();});CHECK(queued->status==Status::ok && !queued->data.empty());
    auto position=[&]{auto entity=client.entity(player);return entity?std::get<double>(std::get<AttributeStruct>(entity->confirmed->values.at("position")).at("x")):0.0;};
    until([&]{return position()==4.0;});
    CHECK(call(player,"arm_timer","").status==Status::ok);
    std::optional<Result> moved;bt.transfer("Player",player.str(),"node-a",[&](Result value){moved=std::move(value);});until([&]{return moved.has_value();});CHECK(moved->status==Status::ok);
    until([&]{return gateway.stats().migrations>=2 && !gateway.stats().migrating;});CHECK(client.entity(player)->handle==original);
    CHECK(call(EntityId(player.str()),"move","2").data=="6.0");
    until([&]{return position()==16.0;});CHECK(decode_entity_record(db->get(entity_record_key(player))->value).snapshot.timers.entries.empty());
    if(argc==3) {
        // Same actor, storage, payload and connections; only addressing differs.
        // Legacy type+id is explicitly exposed solely as a comparison fixture.
        gateway.bind_entity("Player",{"move"});
        for(bool global:{false,true}) {
            constexpr unsigned rounds=2000,concurrency=8;unsigned sent=0,done=0;std::vector<double> latency;
            auto start=std::chrono::steady_clock::now();auto deadline=start+20s;
            while(done<rounds) {
                while(sent<rounds && sent-done<concurrency) {
                    ++sent;auto began=std::chrono::steady_clock::now();
                    auto reply=[&,began](Result result){CHECK(result.status==Status::ok);latency.push_back(std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-began).count());++done;};
                    if(global)client.call(player,"move","0",std::move(reply));
                    else client.request("Player.move",entity_message(player.str(),"0"),std::move(reply));
                }
                pump();CHECK(std::chrono::steady_clock::now()<deadline);std::this_thread::yield();
            }
            auto seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();std::sort(latency.begin(),latency.end());
            std::cout<<"{\"scenario\":\""<<(global?"global_id":"legacy_type_id")<<"\",\"requests\":"<<rounds<<",\"concurrency\":"<<concurrency<<",\"rps\":"<<rounds/seconds<<",\"p50_us\":"<<latency[rounds/2]<<",\"p99_us\":"<<latency[rounds*99/100]<<"}\n";
        }
    }
    CHECK(ae.last_error().empty() && be.last_error().empty());
    other_client.connect({"127.0.0.1",port,{}});until([&]{return other_client.state()==ClientState::connected;});
    std::optional<Result> denied;other_client.call(player,"move","1",[&](Result value){denied=std::move(value);});until([&]{return denied.has_value();});CHECK(denied->status==Status::rejected);
    CHECK(frontend_peers.size()==2);auto replacement=front.identity(frontend_peers.back())->connection;
    std::optional<Result> rebound;rb.bind_owner({},player.str(),replacement,3s,[&](Result value){rebound=std::move(value);});until([&]{return rebound.has_value();});CHECK(rebound->status==Status::ok);
    until([&]{return !client.entity(player);});CHECK(call(player,"move","1").status==Status::rejected);
    denied.reset();other_client.call(player,"move","1",[&](Result value){denied=std::move(value);});until([&]{return denied.has_value();});CHECK(denied->status==Status::ok);
    std::optional<Result> relogged;
    other_client.call(login,"login","Alice renamed",[&](Result value){relogged=std::move(value);});
    until([&]{return relogged.has_value();});CHECK(relogged->status==Status::ok && relogged->data==player.str());
    until([&]{return other_client.controlled() && other_client.controlled()->key.id==player.str();});
    other_client.disconnect();
    auto remote_player=call(manager,"spawn_remote","");if(remote_player.status!=Status::ok)throw std::runtime_error("remote spawn status "+std::to_string(static_cast<unsigned>(remote_player.status))+": "+ae.last_error()+" / "+be.last_error());
    auto remote_id=EntityId(remote_player.data);until([&]{return client.controlled() && client.controlled()->key.id==remote_id.str();});
    CHECK(bh->active("Player",remote_id.str()));CHECK(call(remote_id,"move","1").data=="1.0");
    client.disconnect();ta.stop();tb.stop();ae.stop();be.stop();da.stop();db_delivery.stop();at.stop();bt.stop();ra.stop();rb.stop();gateway.stop();ar.stop();br.stop();ad.stop();bd.stop();ah->stop();bh->stop();a.stop();b.stop();front.stop();back.stop();
    std::cout<<"PASS global flow: Manager, remote LoginEntity creation, Gateway RPC, Player handoff, stable serialized ID and client view\n";
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}

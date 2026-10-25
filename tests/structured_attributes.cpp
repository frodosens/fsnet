#include "fsnet/lua_engine.hpp"
#include "fsnet/entity_host.hpp"
#include "fsnet/client_script_host.hpp"
#include "fsnet/entity_message.hpp"
#include <chrono>
#include <algorithm>
#include <iostream>
#include <thread>
using namespace std::chrono_literals;
#define CHECK(x) do { if(!(x)) throw std::runtime_error(std::string(__FILE__)+':'+std::to_string(__LINE__)+" " #x); } while(false)
const char* model=R"(
    local Vec3=fsnet.struct('Vec3',{x='number',y='number',z='number'})
    local Data=fsnet.struct('Data',{position=Vec3,basis=fsnet.mat4x4,flags=fsnet.array('bool',2),tag='string',code='int32'})
    local Actor=fsnet.Entity:extend('Structured',{owner='legacy_id'})
    Actor:attr('position',{type=Vec3,authority='driver',client='rw',sync='observers'})
    Actor:attr('matrix',{type='mat4x4',authority='driver',client='rw',sync='observers'})
    Actor:attr('data',{type=Data,authority='driver',client='rw',sync='observers'})
    Actor:attr('secret',{type='vec3',client='none',sync='none'})
    Actor:attr('locked',{type='vec3',sync='observers'})
    local cached,foreign
    Actor:rpc('edit',{callers={'client'},access='owner'})
    function Actor:edit()
        local alias=self.position
        self.position={x=4,y=5,z=6}
        alias.x=7 -- field handles address the current property, not an old table
        self.matrix[1][4]=10
        self.data.position.y=8
        self.data.basis[4][4]=1
        self.data.flags[1]=true
        self.data.tag='updated'
        cached=self.position
    end
    Actor:rpc('fail',{callers={'client'},access='owner'})
    function Actor:fail() self.position.x=999; error('rollback') end
    Actor:rpc('scope',{callers={'client'},access='owner'})
    function Actor:scope() assert(not pcall(function() return cached.x end)) end
    function Actor:advance(payload) self.position.x=self.position.x+tonumber(payload); return 'moved' end
    function Actor:bad_prediction()
        self.position.x=77
        pcall(function() foreign.position.x=99 end)
    end
    function Actor:validate_write(proposal)
        return not proposal.position or proposal.position.x<=100
    end
)";
void codec() {
    fsnet::AttributeDefinition vec{fsnet::AttributeType::structure,fsnet::AttributeStruct{{"x",0.0},{"y",0.0},{"z",0.0}}};
    for(auto name:{"x","y","z"}) vec.fields[name]={fsnet::AttributeType::number,0.0};
    fsnet::EntitySchema schema; schema.attributes.emplace("position",vec);
    fsnet::EntityAttributes a(schema,"alice");
    auto next=a.stage({{"position",fsnet::AttributeStruct{{"x",1.0},{"y",2.0},{"z",3.0}}}},{fsnet::Caller::server,{}});
    CHECK(a.publish(next)); auto encoded=fsnet::encode_attributes(a.snapshot({"alice",true}));
    auto decoded=fsnet::decode_attributes(encoded); CHECK(decoded.values==a.all_values());
    CHECK(fsnet::attribute_wire_size(decoded)==encoded.size());
    CHECK(fsnet::attribute_wire_size(decoded.values)==encoded.size());
    fsnet::EntityAttributes restored(schema,"alice"); restored.restore(decoded.values,decoded.revision);
    CHECK(restored.all_values()==a.all_values());
    auto malformed=encoded; auto duplicate=malformed.find(std::string("\1z\1",3));
    CHECK(duplicate!=std::string::npos); malformed[duplicate+1]='x';
    bool duplicate_rejected=false; try { fsnet::decode_attributes(malformed); } catch(...) { duplicate_rejected=true; }
    CHECK(duplicate_rejected);
    auto copied=decoded.values; std::get<fsnet::AttributeStruct>(copied.at("position")).at("x")=8.0;
    CHECK(copied!=decoded.values);
    for(std::size_t n=0;n<encoded.size();++n) { bool failed=false; try { fsnet::decode_attributes(std::string_view(encoded).substr(0,n)); } catch(...) { failed=true; } CHECK(failed); }
    bool failed=false;
    try { a.publish({a.revision(),{{"position",fsnet::AttributeStruct{{"x",1.0},{"y",2.0}}}}}); } catch(...) { failed=true; }
    CHECK(failed && a.all_values()==decoded.values);
    fsnet::AttributeValue deep=0.0;
    for(int i=0;i<10;++i) deep=fsnet::AttributeStruct{{"child",std::move(deep)}};
    failed=false; try { fsnet::encode_attributes({0,0,true,{{"deep",deep}}}); } catch(...) { failed=true; } CHECK(failed);
    fsnet::AttributeValue wide=fsnet::AttributeArray(64,fsnet::AttributeArray(64,0.0));
    failed=false; try { fsnet::encode_attributes({0,0,true,{{"wide",wide}}}); } catch(...) { failed=true; } CHECK(failed);
    std::cout<<"PASS structured native values: schema, deep copies, codec truncation, rollback and bounds\n";
}
int main() {
    try {
        codec();
        fsnet::Node server("struct"),owner("struct"),observer("struct");
        std::vector<std::uint64_t> peers;
        server.identify([&](std::uint64_t peer,const std::string&) {
            if(std::find(peers.begin(),peers.end(),peer)==peers.end()) peers.push_back(peer);
            return std::optional(fsnet::CallIdentity{fsnet::Caller::client,peer==peers.front()?"alice":"bob"});
        });
        auto transport=std::make_shared<fsnet::NodeScriptHost>(server);
        auto entities=std::make_shared<fsnet::EntityScriptHost>(transport,fsnet::memory_database(),"struct/boot");
        fsnet::AttributeReplication replication(server,entities,[](const auto&,const auto&,const auto&) { return true; });
        fsnet::LuaEngine authority(entities); authority.load(std::string(model)+"\nActor:register()\n");
        bool owner_ready=false,observer_ready=false,owner_done=false,observer_done=false;
        fsnet::Reply gate;
        server.bind("gate",[&](std::string,fsnet::Reply reply) { gate=std::move(reply); });
        server.bind("report",[&](std::string data,fsnet::Reply reply) {
            if(data=="owner-ready") owner_ready=true;
            if(data=="observer-ready") observer_ready=true;
            if(data=="owner-done") owner_done=true;
            if(data=="observer-done") observer_done=true;
            reply({fsnet::Status::ok,{}});
        });
        auto port=server.listen("127.0.0.1",0); auto peer=owner.connect("127.0.0.1",port);
        auto oh=std::make_shared<fsnet::ClientScriptHost>(owner,peer);
        {
            fsnet::ScriptLimits limits; limits.memory_bytes=1024*1024; limits.instructions_per_resume=1000000;
            fsnet::LuaEngine bounded(oh,limits);
            bounded.load(std::string(model)+R"(
                Actor:define()
                local value=Actor:spawn_local()
                for i=1,5000 do assert(value.position.x==0) end
                value.position.x=1
                assert(value.position.x==1)
                value:close()
            )");
            bounded.stop(); CHECK(bounded.memory_used()==0);
        }
        fsnet::LuaEngine oe(oh);
        oe.load("local peer="+std::to_string(peer)+"\n"+model+R"(
            Actor:define()
            local first=Actor:spawn_local()
            local second=Actor:spawn_local()
            local source={x=1,y=2,z=3}
            first.position=source; source.x=999
            assert(first.position.x==1 and second.position.x==0)
            second.position=first.position
            first.position.x=9; assert(second.position.x==1)
            first.data.position.z=7
            first.matrix={{1,0,0,0},{0,1,0,0},{0,0,1,0},{0,0,0,1}}
            first.matrix[2][3]=4
            first.data.position={x=1,y=2,z=3}
            assert(#first.matrix==4 and #first.matrix[1]==4)
            local count=0; for key,value in pairs(first.position) do assert(type(key)=='string'); count=count+1 end
            assert(count==3)
            count=0; for _,row in ipairs(first.matrix) do for _,value in ipairs(row) do count=count+1 end end
            assert(count==16)
            assert(not pcall(rawset,first.position,'x',10))
            assert(not pcall(function() first.position.x='bad' end))
            assert(not pcall(function() first.position={x=1,y=2} end))
            assert(not pcall(function() first.position={x=1,y=2,z=3,w=4} end))
            assert(not pcall(function() first.matrix[1][5]=1 end))
            assert(not pcall(function() first.data.code=2147483648 end))
            local stale=first.position; first:close(); assert(not pcall(function() return stale.x end))
            second:close()
            local Bad=fsnet.Entity:extend('TooWide')
            Bad:attr('value',{type=fsnet.array(fsnet.array('number',64),64)})
            assert(not pcall(Bad.define,Bad))
            runtime.start(function()
                local replica=Actor:legacy_watch('alice')
                runtime.request(peer,'report','owner-ready')
                runtime.request(peer,'gate','')
                local reference=Actor:legacy_ref('alice')
                reference:edit('')
                assert(replica.position.x==7 and replica.position.y==5 and replica.matrix[1][4]==10)
                assert(replica.data.position.y==8 and replica.data.flags[1] and replica.data.basis[4][4]==1)
                assert(not pcall(reference.fail,reference,''))
                reference:scope('')
                assert(replica.position.x==7)
                local held=replica.position
                replica.position={x=10,y=20,z=30} -- automatically awaits server validation
                assert(replica.position.x==10 and held.y==20)
                held.x=11 -- nested assignment also proposes and broadcasts automatically
                replica.data.position.x=12
                replica.matrix[2]={1,2,3,4}
                replica.matrix[2][3]=9
                assert(replica.position.x==11 and replica.data.position.x==12 and replica.matrix[2][3]==9)
                assert(not pcall(function() replica.position.x=101 end))
                assert(replica.position.x==11)
                assert(not pcall(function() replica.locked.x=1 end))
                assert(not pcall(function() return replica.secret.x end))
                replica:write({position={x=12,y=20,z=30}})
                assert(replica.position.x==12)
                local prediction=replica:prediction()
                assert(prediction:push(1,'advance','5')=='moved')
                assert(prediction.state.position.x==17 and replica.position.x==12)
                prediction:reconcile(0,replica.revision)
                assert(prediction.state.position.x==17)
                foreign=replica
                assert(not pcall(prediction.push,prediction,2,'bad_prediction',''))
                assert(prediction.state.position.x==17 and replica.position.x==12)
                prediction:close()
                runtime.request(peer,'report','owner-done')
                replica:close()
                assert(not pcall(function() held.x=0 end))
            end)
        )");
        std::shared_ptr<fsnet::ClientScriptHost> bh; std::unique_ptr<fsnet::LuaEngine> be;
        auto pump=[&] { server.poll(); owner.poll(); observer.poll(); entities->poll(); replication.poll(); oh->poll(); authority.poll(); oe.poll(); if(bh) bh->poll(); if(be) be->poll(); };
        auto until=[&](auto done) {
            auto deadline=std::chrono::steady_clock::now()+6s;
            while(!done()) { pump(); if(std::chrono::steady_clock::now()>deadline) throw std::runtime_error("deadline: "+oe.last_error()+" / "+authority.last_error()+" / "+(be?be->last_error():"")); std::this_thread::sleep_for(1ms); }
        };
        until([&]{return owner_ready && bool(gate);});
        auto b=observer.connect("127.0.0.1",port); bh=std::make_shared<fsnet::ClientScriptHost>(observer,b); be=std::make_unique<fsnet::LuaEngine>(bh);
        be->load("local peer="+std::to_string(b)+"\n"+model+R"(
            Actor:define()
            runtime.start(function()
                local replica=Actor:legacy_watch('alice')
                assert(not pcall(function() replica.position.x=50 end)) -- observer has no owner write right
                assert(not pcall(function() return replica.secret.x end))
                runtime.request(peer,'report','observer-ready')
                while replica.position.x~=12 do assert(replica.position.x<=100); runtime.sleep(1) end
                assert(replica.position.y==20 and replica.position.z==30)
                assert(replica.data.position.x==12 and replica.data.position.y==8 and replica.data.flags[1])
                assert(replica.matrix[2][3]==9 and replica.data.tag=='updated')
                runtime.request(peer,'report','observer-done'); replica:close()
            end)
        )");
        until([&]{return observer_ready;}); auto release=std::move(gate); release({fsnet::Status::ok,{}});
        until([&]{return owner_done && observer_done && oe.active()==0 && be->active()==0;});
        CHECK(oe.last_error().empty() && be->last_error().empty());
        oe.stop(); CHECK(oe.memory_used()==0); oh->stop(); be->stop(); CHECK(be->memory_used()==0); bh->stop(); authority.stop();
        std::cout<<"PASS structured Lua attributes: native field views, vec3/matrix, automatic client proposals, observer sync, ACLs and prediction\n";
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}

#include "fsnet/lua_engine.hpp"
#include "fsnet/entity_host.hpp"
#include "fsnet/client_script_host.hpp"
#include "fsnet/entity_message.hpp"
#include <chrono>
#include <iostream>
#include <thread>
using namespace std::chrono_literals;
#define CHECK(x) do { if(!(x)) throw std::runtime_error(std::string(__FILE__)+':'+std::to_string(__LINE__)+" " #x); } while(false)
template<class F> void denied(F f) { bool caught=false; try { f(); } catch(const std::exception&) { caught=true; } CHECK(caught); }
const char* model=R"(
    local Item=fsnet.struct('Item',{id='string',count='int32'})
    local Task=fsnet.struct('Task',{progress='int32',done='bool'})
    local E=fsnet.Entity:extend('Containers',{owner='id'})
    E:attr('items',{type=fsnet.list(Item,4),authority='driver',client='rw',sync='observers'})
    E:attr('tasks',{type=fsnet.map(Task,4),authority='driver',client='rw',sync='observers'})
    E:attr('tags',{type=fsnet.set('string',4),authority='driver',client='rw',sync='observers'})
    E:attr('private_tasks',{type=fsnet.map(Task,4),sync='owner'})
    E:attr('nested',{type=fsnet.map(fsnet.list(Item,4),4),authority='driver',sync='observers'})
    E:attr('phase',{type='int64',default=0,sync='observers'})
    E:rpc('add',{callers={'client'},access='owner'})
    function E:add()
        self.items:add({id='sword',count=1})
        self.items:add({id='potion',count=2})
        self.tasks['daily.1']={progress=0,done=false}
        self.private_tasks['hidden']={progress=3,done=false}
        self.tags:add('ready'); self.tags:add('ready')
        self.nested['bag']={}; self.nested['bag']:add({id='nested',count=1})
        self.phase=1
    end
    E:rpc('change',{callers={'client'},access='owner'})
    function E:change()
        self.items:remove(1); self.items[1].count=5
        self.tasks['daily.1'].progress=10
        self.phase=2
    end
    E:rpc('clear',{callers={'client'},access='owner'})
    function E:clear()
        self.items:clear(); self.tasks['daily.1']=nil; self.tags:remove('ready')
        self.nested['bag']=nil; self.phase=3
    end
    E:rpc('fail',{callers={'client'},access='owner'})
    function E:fail() self.items:add({id='rolled-back',count=1}); error('rollback') end
    function E:validate_write(proposal)
        return not proposal.items or (#proposal.items<=4 and (not proposal.items[1] or proposal.items[1].count<=10))
    end
    function E:predict_add() self.items:add({id='predicted',count=1}) end
)";
void native() {
    using namespace fsnet;
    AttributeDefinition integer{AttributeType::integer,std::int64_t(0)};
    AttributeDefinition list{AttributeType::list,AttributeArray{}}; list.element=std::make_shared<const AttributeDefinition>(integer); list.max_items=2;
    AttributeDefinition map{AttributeType::map,AttributeMap{}}; map.element=list.element; map.max_items=2;
    AttributeDefinition set{AttributeType::set,AttributeArray{}}; set.element=list.element; set.max_items=2;
    EntitySchema schema; schema.attributes={{"list",list},{"map",map},{"set",set}};
    EntityAttributes entity(schema,"alice");
    auto full=entity.snapshot({"alice",true}); auto bytes=encode_attributes(full);
    CHECK(decode_attributes(bytes).values==full.values && attribute_wire_size(full)==bytes.size());
    AttributeReplica replica; CHECK(replica.apply(full));
    CHECK(entity.publish({0,{{"list",AttributeArray{std::int64_t(1),std::int64_t(2)}},{"map",AttributeMap{{"a.b",std::int64_t(7)}}},{"set",AttributeArray{std::int64_t(1)}}}}));
    auto updated=entity.snapshot({"alice",true}); auto encoded=encode_attributes(updated);
    CHECK(decode_attributes(encoded).values==updated.values && attribute_wire_size(updated)==encoded.size());
    CHECK(replica.apply(entity.changes(0,{"alice",true}))); CHECK(replica.values()==entity.all_values());
    EntityAttributes restored(schema,"alice"); restored.restore(decode_attributes(encode_attributes({0,entity.revision(),true,entity.persistent()})).values,entity.revision());
    CHECK(restored.all_values()==entity.all_values());
    auto saved=entity.all_values(); auto revision=entity.revision();
    denied([&]{entity.publish({revision,{{"list",AttributeArray{std::int64_t(1),std::int64_t(2),std::int64_t(3)}}}});});
    denied([&]{entity.publish({revision,{{"map",AttributeMap{{"bad",true}}}}});});
    denied([&]{entity.publish({revision,{{"set",AttributeArray{std::int64_t(1),std::int64_t(1)}}}});});
    denied([&]{entity.publish({revision,{{"map",AttributeMap{{"a",std::int64_t(1)},{"b",std::int64_t(2)},{"c",std::int64_t(3)}}}}});});
    CHECK(entity.all_values()==saved && entity.revision()==revision);
    for(std::size_t n=0;n<encoded.size();++n) denied([&]{decode_attributes(std::string_view(encoded).substr(0,n));});
    auto duplicate=encode_attributes({0,1,true,{{"map",AttributeMap{{"x",true}}}}});
    auto at=duplicate.find(std::string("\6\0\1\1x\2\1",7)); CHECK(at!=std::string::npos);
    duplicate[at+2]=2; duplicate+=duplicate.substr(at+3); denied([&]{decode_attributes(duplicate);});
    AttributeMap bad; bad.emplace(std::string("x\0y",3),true); denied([&]{encode_attributes({0,1,true,{{"map",bad}}});});
    schema.attributes.at("set").element=std::make_shared<const AttributeDefinition>(list); denied([&]{schema.validate();});
}
int main() {
    try {
        native();
        fsnet::Node server("containers"),owner("containers"),observer("containers"); std::uint64_t owner_peer=0;
        server.identify([&](std::uint64_t peer,const std::string&) { if(!owner_peer) owner_peer=peer; return std::optional(fsnet::CallIdentity{fsnet::Caller::client,peer==owner_peer?"alice":"bob"}); });
        auto transport=std::make_shared<fsnet::NodeScriptHost>(server);
        auto entities=std::make_shared<fsnet::EntityScriptHost>(transport,fsnet::memory_database(),"containers/boot");
        fsnet::AttributeReplication replication(server,entities,[](const auto&,const auto&,const auto&){return true;});
        fsnet::LuaEngine authority(entities); authority.load(std::string(model)+"\nE:register()\n");
        bool ready=false,done=false; int observed=0; fsnet::Reply gate;
        server.bind("gate",[&](std::string,fsnet::Reply reply){gate=std::move(reply);});
        server.bind("report",[&](std::string data,fsnet::Reply reply){if(data=="ready") ready=true; if(data=="done") done=true; if(data.size()==1) observed=std::stoi(data); reply({fsnet::Status::ok,{}});});
        auto port=server.listen("127.0.0.1",0); auto peer=owner.connect("127.0.0.1",port);
        auto oh=std::make_shared<fsnet::ClientScriptHost>(owner,peer); fsnet::LuaEngine oe(oh);
        oe.load("local peer="+std::to_string(peer)+"\n"+model+R"(
            E:define()
            assert(not pcall(fsnet.list,'int32',false))
            local a=E:spawn_local(); local b=E:spawn_local()
            assert(#a.items==0 and #a.tasks==0 and #a.tags==0)
            a.items:add({id='a',count=1}); a.items:insert(1,{id='b',count=2})
            assert(a.items[1].id=='b' and #b.items==0)
            a.items[#a.items+1]={id='c',count=3}; a.items[2].count=4
            assert(a.items[2].count==4)
            assert(not pcall(function() a.items[5]={id='hole',count=1} end))
            a.items:add({id='d',count=1}); assert(not pcall(function() a.items:add({id='overflow',count=1}) end)); assert(#a.items==4)
            assert(not pcall(function() a.items:remove(5) end))
            a.items:clear(); assert(#a.items==0)
            a.tasks['a.b']={progress=1,done=false}; a.tasks['add']={progress=2,done=false}
            assert(a.tasks['add'].progress==2 and a.tasks.missing==nil)
            a.tasks['a.b'].done=true; local n=0; for k,v in pairs(a.tasks) do assert(type(k)=='string'); n=n+1 end; assert(n==2)
            a.tasks['a.b']=nil; assert(#a.tasks==1)
            assert(not pcall(function() a.tasks[1]={progress=0,done=false} end))
            assert(not pcall(function() a.tags={'x','x'} end))
            a.tags:add('x'); a.tags:add('x'); assert(#a.tags==1 and a.tags:contains('x'))
            assert(not pcall(function() a.tags[1]='y' end))
            a.tags:remove('x'); a.tags:remove('absent'); assert(#a.tags==0)
            local copied={{id='copy',count=1}}; a.items=copied; copied[1].count=99
            b.items=a.items; a.items[1].count=2; assert(b.items[1].count==1)
            a.nested['bag']={}; a.nested['bag']:add({id='n',count=1}); assert(a.nested['bag'][1].id=='n')
            a:close(); b:close()
            local Wide=fsnet.Entity:extend('WideContainers')
            Wide:attr('grid',{type=fsnet.list(fsnet.list('bool',256),256),authority='driver'})
            Wide:define(); local wide=Wide:spawn_local()
            local grid={}; for i=1,16 do grid[i]={}; for j=1,256 do grid[i][j]=true end end
            assert(not pcall(function() wide.grid=grid end)); assert(#wide.grid==0)
            wide.grid:add({true}); assert(wide.grid[1][1]); wide:close()
            local Invalid=fsnet.Entity:extend('InvalidContainers')
            Invalid:attr('bad',{type=fsnet.set(Item,4)})
            assert(not pcall(Invalid.define,Invalid))
            runtime.start(function()
                local replica=E:watch('alice'); assert(#replica.items==0 and #replica.tasks==0)
                runtime.request(peer,'gate','')
                local ref=E:ref('alice'); ref:add('')
                assert(#replica.items==2 and replica.tasks['daily.1'].progress==0 and #replica.tags==1)
                while runtime.request(peer,'status','')~='1' do runtime.sleep(1) end
                assert(not pcall(ref.fail,ref,'')); assert(#replica.items==2)
                ref:change(''); assert(#replica.items==1 and replica.items[1].count==5)
                while runtime.request(peer,'status','')~='2' do runtime.sleep(1) end
                replica.items:add({id='client',count=1}); assert(#replica.items==2)
                replica.items[1].count=6
                replica.tasks['client.task']={progress=2,done=false}; replica.tasks['client.task'].done=true
                replica.tasks['client.task']=nil
                replica.tags:add('client'); replica.tags:remove('client')
                assert(not pcall(function() replica.items[1].count=11 end)); assert(replica.items[1].count==6)
                local prediction=replica:prediction(); prediction:push(1,'predict_add','')
                assert(#prediction.state.items==3 and #replica.items==2)
                prediction:reconcile(0,replica.revision); assert(#prediction.state.items==3)
                prediction:close()
                ref:clear(''); assert(#replica.items==0 and #replica.tasks==0 and #replica.tags==0 and #replica.nested==0)
                runtime.request(peer,'report','done'); replica:close()
            end)
        )");
        server.bind("status",[&](std::string,fsnet::Reply reply){reply({fsnet::Status::ok,std::to_string(observed)});});
        std::shared_ptr<fsnet::ClientScriptHost> bh; std::unique_ptr<fsnet::LuaEngine> be;
        auto pump=[&]{server.poll(); owner.poll(); observer.poll(); entities->poll(); replication.poll(); oh->poll(); authority.poll(); oe.poll(); if(bh) bh->poll(); if(be) be->poll();};
        auto until=[&](auto condition){auto deadline=std::chrono::steady_clock::now()+8s; while(!condition()){pump(); if(std::chrono::steady_clock::now()>deadline) throw std::runtime_error("deadline "+oe.last_error()+" / "+authority.last_error()+" / "+(be?be->last_error():"")); std::this_thread::sleep_for(1ms);}};
        until([&]{return bool(gate);});
        auto b=observer.connect("127.0.0.1",port); bh=std::make_shared<fsnet::ClientScriptHost>(observer,b); be=std::make_unique<fsnet::LuaEngine>(bh);
        be->load("local peer="+std::to_string(b)+"\n"+model+R"(
            E:define(); runtime.start(function()
                local replica=E:watch('alice')
                assert(not pcall(function() return #replica.private_tasks end))
                assert(not pcall(function() replica.items:add({id='denied',count=1}) end))
                runtime.request(peer,'report','ready')
                while replica.phase~=1 do runtime.sleep(1) end
                assert(#replica.items==2 and replica.items[1].id=='sword' and replica.tasks['daily.1'].progress==0)
                assert(replica.nested['bag'][1].id=='nested' and replica.tags:contains('ready'))
                runtime.request(peer,'report','1')
                while replica.phase~=2 do runtime.sleep(1) end
                assert(#replica.items==1 and replica.items[1].id=='potion' and replica.items[1].count==5 and replica.tasks['daily.1'].progress==10)
                runtime.request(peer,'report','2')
                while replica.phase~=3 do runtime.sleep(1) end
                assert(#replica.items==0 and #replica.tasks==0 and #replica.tags==0 and #replica.nested==0)
                runtime.request(peer,'report','3'); replica:close()
            end)
        )");
        until([&]{return ready;}); auto release=std::move(gate); release({fsnet::Status::ok,{}});
        until([&]{return done && observed==3 && !oe.active() && !be->active();});
        CHECK(oe.last_error().empty() && be->last_error().empty());
        oe.stop(); CHECK(oe.memory_used()==0); be->stop(); CHECK(be->memory_used()==0); authority.stop(); oh->stop(); bh->stop();
        std::cout<<"PASS dynamic containers: codec, persistence, limits, rollback, nested edits, automatic proposals, observer sync and prediction\n";
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n'; return 1;}
}

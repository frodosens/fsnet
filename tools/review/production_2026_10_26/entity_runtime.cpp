#include "fsnet/entity_host.hpp"
#include "fsnet/entity_message.hpp"
#include <thread>
#include <iostream>
#include <unordered_map>
using namespace fsnet;using namespace std::chrono_literals;
struct Parent:ScriptHost {
 std::unordered_map<std::string,ContextHandler> routes;
 bool has_route(const std::string& name)const override{return routes.contains(name);}
 void bind(std::string,Handler)override{throw std::logic_error("context");}
 void bind_context(std::string name,RpcPolicy,ContextHandler fn)override{routes.emplace(std::move(name),std::move(fn));}
 void unbind(const std::string& name)override{routes.erase(name);}
 std::uint64_t connect(const std::string&,std::uint16_t)override{return 0;}
 void call(std::uint64_t,std::string,std::string,std::chrono::milliseconds,Reply fn)override{fn({Status::not_found,{}});}
};
struct Fixture {
 std::shared_ptr<Parent> parent=std::make_shared<Parent>();std::shared_ptr<Storage> db=memory_database();
 EntityRuntimeLimits limits;std::shared_ptr<EntityScriptHost> host;
 Fixture(){limits.request_timeout=20ms;limits.lease=200ms;host=std::make_shared<EntityScriptHost>(parent,db,"probe/boot",limits);}
 void pump(){host->poll();std::this_thread::sleep_for(1ms);}
 template<class F> void until(F f){auto deadline=std::chrono::steady_clock::now()+2s;while(!f()){pump();if(std::chrono::steady_clock::now()>deadline)throw std::runtime_error("deadline");}}
 template<class F> Result wait(F f){std::optional<Result> r;f([&](Result x){r=std::move(x);});until([&]{return bool(r);});return *r;}
 EntityId create(EntityPersistence p){auto r=wait([&](Reply fn){host->create_entity("Actor",{},{"probe","actor",{},"probe",p},std::move(fn));});if(r.status!=Status::ok)throw std::runtime_error("create");return EntityId(r.data);}
 Result call(EntityId id,std::string method,std::string receipt={}){return wait([&](Reply fn){host->call_entity("@",entity_call_message(id,method,"",receipt),1s,std::move(fn));});}
 ~Fixture(){host->stop();}
};
void replay(bool rebuildable){Fixture f;EntitySchema schema;f.host->define_schema("Actor",schema);auto initial=encode_attributes({0,0,true,{}});unsigned executed=0;
 f.host->bind_stateful_context("Actor.hit","Actor",initial,{false,true,false},[&](const CallIdentity&,std::string input,Reply fn){auto call=parse_entity_message(input);auto state=parse_state_message(call->payload);++executed;fn({Status::ok,state_message(state->state,std::to_string(executed))});});
 auto id=f.create(rebuildable?EntityPersistence::rebuildable:EntityPersistence::durable);auto a=f.call(id,"hit","same");auto b=f.call(id,"hit","same");
 std::cout<<"rebuildable="<<rebuildable<<" first="<<a.data<<" retry="<<b.data<<" executions="<<executed<<" replays="<<f.host->stats().replayed<<'\n';
}
void timeout(){Fixture f;EntitySchema schema;f.host->define_schema("Actor",schema);auto initial=encode_attributes({0,0,true,{}});bool entered=false,completed=false;
 f.host->bind_stateful_context("Actor.hang","Actor",initial,{false,true,false},[&](const CallIdentity&,std::string,Reply){entered=true;});
 auto id=f.create(EntityPersistence::durable);f.host->call_entity("@",entity_call_message(id,"hang",""),1s,[&](Result){completed=true;});f.until([&]{return entered;});
 auto deadline=std::chrono::steady_clock::now()+100ms;while(std::chrono::steady_clock::now()<deadline)f.pump();
 auto handle=f.host->handle("Actor",id.str());auto off=f.wait([&](Reply fn){f.host->deactivate(*handle,std::move(fn));});
 std::cout<<"native_deadline_ms=20 elapsed_ms=100 callback="<<completed<<" pending="<<f.host->stats().pending<<" deactivate_status="<<unsigned(off.status)<<'\n';f.host->stop();
}
int main(){try{replay(false);replay(true);timeout();}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}

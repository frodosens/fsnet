// Review diagnostics, originally reproducing defects on baseline 130ef7a.
// Build/run via tools/review/run_runtime_review.py; regressions live in CTest.
#include "fsnet/lua_engine.hpp"
#include "fsnet/entity_host.hpp"
#include "fsnet/entity_record.hpp"
#include "fsnet/entity_message.hpp"
#include "fsnet/attribute_replication.hpp"
#include <chrono>
#include <fstream>
#include <iostream>
#include <thread>
#include <unordered_map>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <vector>
using namespace std::chrono_literals;
struct Host:fsnet::ScriptHost{
 std::unordered_map<std::string,fsnet::Handler> handlers;
 std::unordered_map<std::string,fsnet::ContextHandler> contexts;
 std::unordered_map<std::string,fsnet::RpcPolicy> policies;
 bool has_route(const std::string& r)const override{return handlers.contains(r)||contexts.contains(r);}
 void bind(std::string r,fsnet::Handler h)override{handlers.emplace(std::move(r),std::move(h));}
 void bind_context(std::string r,fsnet::RpcPolicy p,fsnet::ContextHandler h)override{policies[r]=p;contexts.emplace(std::move(r),std::move(h));}
 void rpc_policy(const std::string& r,fsnet::RpcPolicy p)override{policies[r]=p;}
 void unbind(const std::string&r)override{handlers.erase(r);contexts.erase(r);}
 std::uint64_t connect(const std::string&,std::uint16_t)override{return 1;}
 void call(std::uint64_t,std::string,std::string,std::chrono::milliseconds,fsnet::Reply r)override{r({fsnet::Status::ok,{}});}
};
template<class F,class Done>void pump(F f,Done done){auto end=std::chrono::steady_clock::now()+3s;while(!done()){f();if(std::chrono::steady_clock::now()>end)throw std::runtime_error("deadline");std::this_thread::sleep_for(1ms);}}
int main(int argc,char**argv){try{
 std::string mode=argc>1?argv[1]:"lua";
 if(mode=="lua"){
  if(argc<3)throw std::invalid_argument("lua mode requires a script path");
  std::ifstream file(argv[2]);if(!file)throw std::runtime_error("cannot open script");std::string script((std::istreambuf_iterator<char>(file)),{});auto host=std::make_shared<Host>();fsnet::ScriptLimits limits;limits.instructions_per_resume=1000;limits.task_timeout=10ms;
  fsnet::LuaEngine engine(host,limits);engine.load(script);std::optional<fsnet::Result> result;
  auto start=std::chrono::steady_clock::now();host->handlers.at("probe")("",[&](auto r){result=std::move(r);});pump([&]{engine.poll();},[&]{return result.has_value();});
  std::cout<<"status="<<static_cast<int>(result->status)<<" elapsed_ms="<<std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count()<<" result="<<result->data<<" error="<<engine.last_error()<<'\n';
 }else if(mode=="memory"){
  auto host=std::make_shared<Host>();fsnet::ScriptLimits limits;limits.memory_bytes=512*1024;limits.task_timeout=30000ms;fsnet::LuaEngine engine(host,limits);
  engine.load("runtime.rpc('probe',function(data) data=nil; runtime.sleep(30000);return '' end)");
  auto rss=[](){std::ifstream f("/proc/self/status");std::string line;while(std::getline(f,line))if(line.starts_with("VmRSS:"))return line;return std::string{};};
  auto before=rss();unsigned completed=0;
  for(int i=0;i<64;++i){host->handlers.at("probe")(std::string(262144,'x'),[&](auto){++completed;});engine.poll();}
  std::cout<<"active="<<engine.active()<<" completed="<<completed<<" vm_bytes="<<engine.memory_used()<<" before="<<before<<" after="<<rss()<<'\n';
 }else if(mode=="uncertain"){
  struct Store:fsnet::Storage{std::shared_ptr<fsnet::Storage> base=fsnet::memory_database();bool armed=false;std::optional<fsnet::Record>get(std::string_view k)override{return base->get(k);}bool erase(std::string_view k,std::uint64_t v)override{return base->erase(k,v);}bool compare_exchange(std::string_view k,std::uint64_t v,std::string_view bytes)override{bool ok=base->compare_exchange(k,v,bytes);if(ok&&armed&&k==fsnet::entity_record_key("Player","bob")){auto d=fsnet::decode_entity_record(bytes).snapshot.delivery;if(!d.inbox.empty()){armed=false;throw std::runtime_error("commit succeeded; response lost");}}return ok;}};
  auto db=std::make_shared<Store>();auto parent=std::make_shared<Host>();auto host=std::make_shared<fsnet::EntityScriptHost>(parent,db,"owner/boot");host->enable_delivery(true);fsnet::LuaEngine engine(host);engine.load("local P=fsnet.Entity:extend('Player',{owner='id'});P:attr('gold',{type='int32',default=0,sync='owner'});P:rpc('reward',{callers={'server'}});function P:reward()self.gold=self.gold+1;return tostring(self.gold)end;P:register()");
  auto wait=[&](auto start){std::optional<fsnet::Result> result;start([&](auto r){result=std::move(r);});pump([&]{host->poll();engine.poll();},[&]{return result.has_value();});return *result;};wait([&](auto cb){host->read_attributes("Player","bob",cb);});unsigned events=0;host->on_attributes([&](const auto&){++events;});auto target=fsnet::decode_entity_record(db->get(fsnet::entity_record_key("Player","bob"))->value);
  fsnet::DeliveryEnvelope e;e.source_type="Quest";e.source_id="alice";e.source_epoch=123;e.message.type="Player";e.message.id="bob";e.message.method="reward";e.message.target_epoch=target.snapshot.delivery.epoch;e.message.sequence=1;e.message.ticket=fsnet::delivery_ticket(e);fsnet::EntityRecord source;source.snapshot.delivery.epoch=123;source.snapshot.delivery.outbox.push_back(e.message);source.snapshot.delivery.sent[fsnet::delivery_channel("Player","bob",e.message.target_epoch)]=1;db->compare_exchange(fsnet::entity_record_key("Quest","alice"),0,fsnet::encode_entity_record(source));db->armed=true;
  unsigned invalidations=0;
  host->on_closed([&](const auto&,const auto&,bool){++invalidations;});
  auto first=wait([&](auto cb){host->receive_delivery({fsnet::Caller::server,"service"},e,cb);});
  auto retry=wait([&](auto cb){host->receive_delivery({fsnet::Caller::server,"service"},e,cb);});
  auto read=wait([&](auto cb){host->read_attributes("Player","bob",cb);});
  auto gold=std::get<std::int64_t>(fsnet::decode_attributes(read.data).values.at("gold"));
  std::cout<<"first_status="<<static_cast<int>(first.status)<<" retry_status="<<static_cast<int>(retry.status)
    <<" stored_gold="<<gold<<" attribute_events="<<events<<" invalidations="<<invalidations<<'\n';
 }else if(mode=="destroy"){
  fsnet::Node server("token");auto client=std::make_unique<fsnet::Node>("token");auto port=server.listen("127.0.0.1",0);server.bind("echo",[](std::string data,auto reply){reply({fsnet::Status::ok,std::move(data)});});auto peer=client->connect("127.0.0.1",port);bool done=false;client->call(peer,"echo","x",1s,[&](auto){client.reset();done=true;});pump([&]{server.poll();if(client)client->poll();},[&]{return done;});std::cout<<"destroy_callback_completed="<<done<<'\n';
 }else if(mode=="replay"){
  auto db=fsnet::memory_database();auto pa=std::make_shared<Host>(),pb=std::make_shared<Host>();auto a=std::make_shared<fsnet::EntityScriptHost>(pa,db,"a/boot"),b=std::make_shared<fsnet::EntityScriptHost>(pb,db,"b/boot");
  fsnet::LuaEngine ae(a),be(b);std::string script="local A=fsnet.Entity:extend('Actor',{owner='id'});A:attr('activations',{type='int32',default=0,sync='owner'});function A:on_activate()self.activations=self.activations+1 end;A:rpc('get',{callers={'server'}});function A:get()return tostring(self.activations)end;A:register()";ae.load(script);be.load(script);
  auto invoke=[&](auto p,auto host,auto& engine,std::string token){std::optional<fsnet::Result> result;p->contexts.at("Actor.get")({fsnet::Caller::server,"service"},fsnet::entity_message("alice","",token),[&](auto r){result=std::move(r);});pump([&]{host->poll();engine.poll();},[&]{return result.has_value();});return result->data;};
  auto first=invoke(pa,a,ae,"one");std::optional<fsnet::Result> release;a->deactivate(*a->handle("Actor","alice"),[&](auto r){release=std::move(r);});pump([&]{a->poll();ae.poll();},[&]{return release.has_value();});
  auto replay=invoke(pb,b,be,"one");auto active=b->active("Actor","alice");auto next=invoke(pb,b,be,"");std::cout<<"first="<<first<<" replay="<<replay<<" active_after_replay="<<active<<" next="<<next<<" expected_next=2\n";
 }else if(mode=="frame"){
  fsnet::Node server("token");int authorized=0;server.authorize([&](auto,const auto&,const auto&){++authorized;return false;});auto port=server.listen("127.0.0.1",0);
  auto rss=[](){std::ifstream f("/proc/self/status");std::string line;while(std::getline(f,line))if(line.starts_with("VmRSS:"))return line;return std::string{};};auto before=rss();std::vector<int> sockets;
  for(int i=0;i<64;++i){int fd=::socket(AF_INET,SOCK_STREAM,0);if(fd<0)throw std::runtime_error("socket");sockaddr_in address{};address.sin_family=AF_INET;address.sin_port=htons(port);address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);if(::connect(fd,reinterpret_cast<sockaddr*>(&address),sizeof(address)))throw std::runtime_error("connect");sockets.push_back(fd);unsigned char header[]={0,16,0,0};::send(fd,header,4,0);for(int j=0;j<4;++j){server.poll();std::this_thread::sleep_for(1ms);}}
  std::cout<<"connections="<<server.stats().connections<<" authorize_calls="<<authorized<<" before="<<before<<" after="<<rss()<<'\n';for(auto fd:sockets)::close(fd);
 }else if(mode=="policy"){
  auto parent=std::make_shared<Host>();auto db=fsnet::memory_database();auto host=std::make_shared<fsnet::EntityScriptHost>(parent,db,"owner/boot");host->enable_delivery(true);
  fsnet::EntitySchema schema;schema.attributes.emplace("gold",fsnet::AttributeDefinition{fsnet::AttributeType::integer,std::int64_t(0)});host->define_schema("Player",schema);
  int applied=0;host->bind_stateful_context("Player.reward","Player",fsnet::encode_attributes({0,0,true,fsnet::EntityAttributes(schema,"").persistent()}),{false,true,false},[&](auto&,std::string input,auto reply){++applied;auto e=fsnet::parse_entity_message(input);auto s=fsnet::parse_state_message(e->payload);reply({fsnet::Status::ok,fsnet::state_message(s->state,"awarded")});});
  std::optional<fsnet::Result> result;host->read_attributes("Player","bob",[&](auto r){result=std::move(r);});pump([&]{host->poll();},[&]{return result.has_value();});
  auto target=fsnet::decode_entity_record(db->get(fsnet::entity_record_key("Player","bob"))->value);fsnet::DeliveryEnvelope e;e.source_type="Quest";e.source_id="alice";e.source_epoch=123;e.message.type="Player";e.message.id="bob";e.message.method="reward";e.message.target_epoch=target.snapshot.delivery.epoch;e.message.sequence=1;e.message.ticket=fsnet::delivery_ticket(e);
  fsnet::EntityRecord source;source.snapshot.delivery.epoch=123;source.snapshot.delivery.outbox.push_back(e.message);source.snapshot.delivery.sent[fsnet::delivery_channel("Player","bob",e.message.target_epoch)]=1;db->compare_exchange(fsnet::entity_record_key("Quest","alice"),0,fsnet::encode_entity_record(source));
  host->rpc_policy("Player.reward",{true,false,false});result.reset();host->receive_delivery({fsnet::Caller::server,"trusted-owner"},e,[&](auto r){result=std::move(r);});pump([&]{host->poll();},[&]{return result.has_value();});
  std::cout<<"transport_server_allowed="<<parent->policies.at("Player.reward").server<<" handler_executed="<<applied<<" status="<<static_cast<int>(result->status)<<'\n';
 }else{throw std::invalid_argument("unknown probe mode");}
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 2;}}

#include "fsnet/entity_host.hpp"
#include "fsnet/entity_message.hpp"
#include <iostream>
#include <thread>
#include <unordered_map>
using namespace std::chrono_literals;
struct Parent:fsnet::ScriptHost {
 std::unordered_map<std::string,fsnet::Handler> handlers;
 bool has_route(const std::string&s)const override{return handlers.contains(s);}
 void bind(std::string s,fsnet::Handler h)override{handlers.emplace(std::move(s),std::move(h));}
 void unbind(const std::string&s)override{handlers.erase(s);}
 std::uint64_t connect(const std::string&,std::uint16_t)override{return 0;}
 void call(std::uint64_t,std::string,std::string,std::chrono::milliseconds,fsnet::Reply)override{}
};
int main(int argc,char**){
 auto parent=std::make_shared<Parent>();fsnet::EntityRuntimeLimits limits;limits.requests=1;limits.idle_timeout=1h;
 fsnet::EntityScriptHost host(parent,fsnet::memory_database(),"owner/boot",limits);
 auto noop=[](std::string input,fsnet::Reply reply){auto s=fsnet::parse_state_message(fsnet::parse_entity_message(input)->payload);reply({fsnet::Status::ok,fsnet::state_message(s->state,"")});};
 host.bind_stateful("Actor.read","Actor","",noop);host.bind_stateful("Actor.$deactivate","Actor","",noop);
 fsnet::Reply held;
 host.bind_stateful("Actor.hold","Actor","",[&](std::string,fsnet::Reply reply){held=std::move(reply);});
 auto pump=[&](auto done){auto end=std::chrono::steady_clock::now()+1s;while(!done()){host.poll();if(std::chrono::steady_clock::now()>end)throw std::runtime_error("timeout");std::this_thread::sleep_for(1ms);}};
 auto invoke=[&](std::string method,std::string id){std::optional<fsnet::Result> r;parent->handlers.at("Actor."+method)(fsnet::entity_message(id,""),[&](auto value){r=std::move(value);});pump([&]{return bool(r);});return *r;};
 invoke("read","a");invoke("read","b");auto handle=*host.handle("Actor","b");
 if(argc>1){
  std::optional<fsnet::Result> first_drain;host.drain([&](auto r){first_drain=std::move(r);});pump([&]{return bool(first_drain);});
  std::optional<fsnet::Result> retry_drain;host.drain([&](auto r){retry_drain=std::move(r);});pump([&]{return bool(retry_drain);});
  std::cout<<"first_drain="<<int(first_drain->status)<<" retry_drain="<<int(retry_drain->status)<<" entities="<<host.stats().entities<<" pending="<<host.stats().pending<<"\n";return 0;
 }
 std::optional<fsnet::Result> first;parent->handlers.at("Actor.hold")(fsnet::entity_message("a",""),[&](auto value){first=std::move(value);});pump([&]{return bool(held);});
 std::optional<fsnet::Result> unload;host.deactivate(handle,[&](auto value){unload=std::move(value);});
 held({fsnet::Status::ok,fsnet::state_message("","")});pump([&]{return bool(first);});
 auto retry=invoke("read","b");std::optional<fsnet::Result> unload_retry;host.deactivate(handle,[&](auto value){unload_retry=std::move(value);});
 pump([&]{return bool(unload_retry);});
 std::cout<<"unload="<<int(unload->status)<<" pending_after_release="<<host.stats().pending<<" read_after_release="<<int(retry.status)<<" unload_retry="<<int(unload_retry->status)<<" active_b="<<host.active("Actor","b")<<"\n";
}

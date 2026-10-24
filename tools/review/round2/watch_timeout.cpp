#include "fsnet/lua_engine.hpp"
#include <iostream>
#include <thread>
#include <unordered_map>
using namespace std::chrono_literals;
struct Host:fsnet::ScriptHost{
 std::unordered_map<std::string,fsnet::Handler> handlers; AttributeReady pending;
 unsigned unwatches=0;
 bool has_route(const std::string&s)const override{return handlers.contains(s);}
 void bind(std::string s,fsnet::Handler h)override{handlers.emplace(std::move(s),std::move(h));}
 void unbind(const std::string&s)override{handlers.erase(s);}
 uint64_t connect(const std::string&,uint16_t)override{return 1;}
 void call(uint64_t,std::string,std::string,std::chrono::milliseconds,fsnet::Reply r)override{r({fsnet::Status::ok,{}});}
 void watch_attributes(std::string,std::string,AttributeReady f,std::chrono::milliseconds)override{pending=std::move(f);}
 void unwatch_attributes(uint64_t)override{++unwatches;}
};
int main(){auto h=std::make_shared<Host>();fsnet::ScriptLimits l;l.task_timeout=10ms;fsnet::LuaEngine e(h,l);e.load(R"(
local C=fsnet.Entity:extend('Watch')
C:attr('x',{type='int32',default=0})
C:define()
runtime.start(function() C:watch('alice') end)
)");e.poll();std::cout<<"watch_pending="<<bool(h->pending)<<" active="<<e.active()<<'\n';h->pending(fsnet::Status::ok,123);std::this_thread::sleep_for(20ms);e.poll();std::cout<<"after_timeout active="<<e.active()<<" unwatches="<<h->unwatches<<" error="<<e.last_error()<<'\n';e.stop();std::cout<<"after_stop unwatches="<<h->unwatches<<'\n';}

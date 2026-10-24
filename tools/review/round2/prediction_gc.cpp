#include "fsnet/lua_engine.hpp"
#include <sys/resource.h>
#include <iostream>
#include <fstream>
#include <sstream>
#include <unordered_map>
struct Host:fsnet::ScriptHost {
 std::unordered_map<std::string,fsnet::Handler> handlers;
 bool has_route(const std::string& s)const override{return handlers.contains(s);}
 void bind(std::string s,fsnet::Handler h)override{handlers.emplace(std::move(s),std::move(h));}
 void unbind(const std::string&s)override{handlers.erase(s);}
 void watch_attributes(std::string,std::string,AttributeReady f,std::chrono::milliseconds)override{f(fsnet::Status::ok,1);}
 std::shared_ptr<const fsnet::AttributeUpdate> attribute_snapshot(uint64_t)const override{return std::make_shared<const fsnet::AttributeUpdate>(fsnet::AttributeUpdate{0,0,true,{{"value",fsnet::AttributeArray(15,fsnet::AttributeArray(256,std::int64_t(0)))}}});}
 uint64_t connect(const std::string&,uint16_t)override{return 1;}
 void call(uint64_t,std::string,std::string,std::chrono::milliseconds,fsnet::Reply r)override{r({fsnet::Status::ok,{}});}
};
long rss(){rusage r{};getrusage(RUSAGE_SELF,&r);return r.ru_maxrss;}
int main(int argc,char**argv){auto h=std::make_shared<Host>();fsnet::ScriptLimits l;l.memory_bytes=32*1024*1024;fsnet::LuaEngine e(h,l);std::ifstream f(argv[1]);std::stringstream s;s<<f.rdbuf();auto before=rss();try{e.load(s.str());std::cout<<"loaded ";}catch(std::exception&x){std::cout<<"failed="<<x.what()<<' ';}std::cout<<"vm="<<e.memory_used()<<" rss_before_kib="<<before<<" rss_after_kib="<<rss()<<'\n';for(int i=0;i<10;++i)e.poll();std::cout<<"after_poll_vm="<<e.memory_used()<<" active="<<e.active()<<" error="<<e.last_error()<<char(10);e.stop();std::cout<<"stopped_vm="<<e.memory_used()<<" rss_after_stop_kib="<<rss()<<'\n';}

#include "fsnet/entity_host.hpp"
#include "fsnet/entity_message.hpp"
#include <iostream>
#include <thread>
#include <unordered_map>
using namespace std::chrono_literals;
struct Host:fsnet::ScriptHost {
 std::unordered_map<std::string,fsnet::Handler> handlers;
 bool has_route(const std::string& r)const override{return handlers.contains(r);}
 void bind(std::string r,fsnet::Handler h)override{handlers.emplace(std::move(r),std::move(h));}
 void unbind(const std::string& r)override{handlers.erase(r);}
 std::uint64_t connect(const std::string&,std::uint16_t)override{return 1;}
 void call(std::uint64_t,std::string,std::string,std::chrono::milliseconds,fsnet::Reply)override{throw std::logic_error("unused");}
};
int main(){auto p=std::make_shared<Host>();fsnet::EntityScriptHost host(p,fsnet::memory_database(),"owner");
 host.bind_legacy_stateful("Actor.echo","Actor","",[](auto input,auto done){done({fsnet::Status::ok,std::string(fsnet::parse_entity_message_view(input)->payload)});});
 std::size_t completed=0,success=0,overloaded=0;
 for(unsigned i=0;i<64;++i)p->handlers.at("Actor.echo")(fsnet::entity_message(std::to_string(i),""),[&](auto result){++completed;if(result.status==fsnet::Status::ok)++success;if(result.status==fsnet::Status::overloaded)++overloaded;});
 auto pending=host.stats().pending;host.poll();std::cout<<"{\"submitted_pending\":"<<pending<<",\"first_poll_completed\":"<<completed<<",\"first_poll_overloaded\":"<<overloaded<<"}"<<std::endl;
 auto deadline=std::chrono::steady_clock::now()+3s;while(completed<64&&std::chrono::steady_clock::now()<deadline){host.poll();std::this_thread::sleep_for(1ms);}
 std::cout<<"{\"final_completed\":"<<completed<<",\"success\":"<<success<<",\"overloaded\":"<<overloaded<<"}"<<std::endl;
}

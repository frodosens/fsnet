#include "fsnet/entity_host.hpp"
#include "fsnet/entity_message.hpp"
#include "fsnet/entity_record.hpp"
#include <future>
#include <iostream>
#include <thread>
#include <unordered_map>
using namespace std::chrono_literals;
#define CHECK(x) do{if(!(x))throw std::runtime_error(#x);}while(false)
struct Parent:fsnet::ScriptHost {
    std::unordered_map<std::string,fsnet::Handler> handlers;
    bool has_route(const std::string& s)const override{return handlers.contains(s);}
    void bind(std::string s,fsnet::Handler h)override{handlers.emplace(std::move(s),std::move(h));}
    void unbind(const std::string& s)override{handlers.erase(s);}
    std::uint64_t connect(const std::string&,std::uint16_t)override{return 0;}
    void call(std::uint64_t,std::string,std::string,std::chrono::milliseconds,fsnet::Reply)override{}
};
template<class Start> fsnet::Result wait(fsnet::EntityScriptHost& host,Start start){
    std::optional<fsnet::Result> result;start([&](auto r){result=std::move(r);});
    auto end=std::chrono::steady_clock::now()+3s;
    while(!result){host.poll();CHECK(std::chrono::steady_clock::now()<end);std::this_thread::sleep_for(1ms);}
    return *result;
}
auto noop=[](std::string input,fsnet::Reply reply){
    auto value=fsnet::parse_state_message(fsnet::parse_entity_message(input)->payload);
    reply({fsnet::Status::ok,fsnet::state_message(value->state,"")});
};
void admission(){
    auto parent=std::make_shared<Parent>();fsnet::EntityRuntimeLimits limits;limits.requests=1;
    fsnet::EntityScriptHost host(parent,fsnet::memory_database(),"owner/boot",limits);
    host.bind_stateful("Actor.read","Actor","",noop);host.bind_stateful("Actor.$deactivate","Actor","",noop);
    host.bind_stateful("Actor.$destroy","Actor","",noop);
    for(auto id:{"a","b"})CHECK(wait(host,[&](auto cb){parent->handlers.at("Actor.read")(fsnet::entity_message(id,""),cb);}).status==fsnet::Status::ok);
    auto first=wait(host,[&](auto cb){host.drain(cb);});CHECK(first.status==fsnet::Status::overloaded);
    CHECK(wait(host,[&](auto cb){host.drain(cb);}).status==fsnet::Status::ok);
    CHECK(host.stats().entities==0 && host.stats().pending==0);
}
void burst(){
    auto parent=std::make_shared<Parent>();fsnet::EntityScriptHost host(parent,fsnet::memory_database(),"owner/boot");
    host.bind_stateful("Actor.read","Actor","",noop);unsigned completed=0;
    for(unsigned i=0;i<64;++i)parent->handlers.at("Actor.read")(fsnet::entity_message(std::to_string(i),""),[&](auto r){CHECK(r.status==fsnet::Status::ok);++completed;});
    auto end=std::chrono::steady_clock::now()+3s;
    while(completed<64){host.poll();CHECK(std::chrono::steady_clock::now()<end);std::this_thread::sleep_for(1ms);}
    CHECK(host.stats().pending==0);
}
void expired_control(){
    for(int action=0;action<3;++action){
        auto parent=std::make_shared<Parent>();fsnet::EntityRuntimeLimits limits;limits.request_timeout=100ms;limits.lease=1s;
        fsnet::EntityScriptHost host(parent,fsnet::memory_database(),"owner/boot",limits);
        host.bind_stateful("Actor.read","Actor","",noop);host.bind_stateful("Actor.$deactivate","Actor","",noop);host.bind_stateful("Actor.$destroy","Actor","",noop);
        CHECK(wait(host,[&](auto cb){parent->handlers.at("Actor.read")(fsnet::entity_message("a",""),cb);}).status==fsnet::Status::ok);
        auto handle=*host.handle("Actor","a");
        auto control=[&](auto cb){if(action==0)host.deactivate(handle,cb);else if(action==1)host.control_entity("Actor","a",true,cb);else host.handoff(handle,"other",cb);};
        std::optional<fsnet::Result> expired;control([&](auto result){expired=std::move(result);});
        std::this_thread::sleep_for(150ms);host.poll();
        CHECK(expired&&expired->status==fsnet::Status::timeout&&host.active("Actor","a"));
        CHECK(wait(host,control).status==fsnet::Status::ok&&host.stats().pending==0);
    }
}
void opaque_delivery(){
    auto parent=std::make_shared<Parent>();auto db=fsnet::memory_database();fsnet::EntityScriptHost host(parent,db,"owner/boot");host.enable_delivery(true);
    host.bind_stateful("Target.reward","Target","opaque",noop);
    host.bind_stateful("Source.issue","Source","opaque",[&](std::string input,fsnet::Reply reply){
        auto state=fsnet::parse_state_message(fsnet::parse_entity_message(input)->payload);
        host.send_entity("Source","a","Target","b","reward","",[state,reply](auto r){CHECK(r.status==fsnet::Status::ok);reply({r.status,fsnet::state_message(state->state,r.data)});});
    });
    CHECK(wait(host,[&](auto cb){parent->handlers.at("Target.reward")(fsnet::entity_message("b",""),cb);}).status==fsnet::Status::ok);
    CHECK(wait(host,[&](auto cb){parent->handlers.at("Source.issue")(fsnet::entity_message("a",""),cb);}).status==fsnet::Status::ok);
    auto source=fsnet::decode_entity_record(db->get(fsnet::entity_record_key("Source","a"))->value);
    auto received=wait(host,[&](auto cb){host.receive_delivery({fsnet::Caller::server,"owner"},{"Source","a",source.snapshot.delivery.epoch,source.snapshot.delivery.outbox.front()},cb);});
    CHECK(received.status==fsnet::Status::ok);auto [ticket,value]=fsnet::decode_delivery_ack(received.data);CHECK(value.status==fsnet::Status::ok);
    CHECK(wait(host,[&](auto cb){host.acknowledge_delivery("Source","a",source.snapshot.delivery.epoch,ticket,value,cb);}).status==fsnet::Status::ok);
    CHECK(fsnet::decode_entity_record(db->get(fsnet::entity_record_key("Source","a"))->value).snapshot.delivery.outbox.front().done);
}
void shutdown(){
    struct Blocking:fsnet::Storage {
        std::shared_ptr<fsnet::Storage> base=fsnet::memory_database();std::promise<void> entered,release;
        std::shared_future<void> gate=release.get_future().share();bool first=true;
        std::optional<fsnet::Record> get(std::string_view key)override{if(first){first=false;entered.set_value();gate.wait();}return base->get(key);}
        bool compare_exchange(std::string_view k,std::uint64_t v,std::string_view b)override{return base->compare_exchange(k,v,b);}
        bool erase(std::string_view k,std::uint64_t v)override{return base->erase(k,v);}
    };
    auto parent=std::make_shared<Parent>();auto db=std::make_shared<Blocking>();fsnet::EntityScriptHost host(parent,db,"owner/boot");host.bind_stateful("Actor.read","Actor","",noop);
    unsigned replies=0;parent->handlers.at("Actor.read")(fsnet::entity_message("a",""),[&](auto r){CHECK(r.status==fsnet::Status::disconnected);++replies;});host.poll();
    CHECK(db->entered.get_future().wait_for(2s)==std::future_status::ready);
    std::thread release([&]{std::this_thread::sleep_for(10ms);db->release.set_value();});host.stop();release.join();CHECK(replies==1&&host.stats().pending==0);
}
void throwing_completion(){
    fsnet::Node node("exception");auto peer=node.connect("127.0.0.1",9);unsigned completions=0;
    for(unsigned i=0;i<3;++i)node.call(peer,"echo","",1s,[&](auto r){CHECK(r.status==fsnet::Status::disconnected);++completions;throw std::runtime_error("application error");});
    node.stop();CHECK(completions==3&&node.pending()==0&&node.stats().callback_failures==3);
}
int main(){try{shutdown();admission();expired_control();burst();opaque_delivery();throwing_completion();std::cout<<"PASS runtime shutdown, admission, burst fairness, opaque delivery and callback containment\n";}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}

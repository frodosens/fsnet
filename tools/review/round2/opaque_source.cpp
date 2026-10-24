#include "fsnet/entity_host.hpp"
#include "fsnet/entity_message.hpp"
#include "fsnet/entity_record.hpp"
#include <iostream>
#include <thread>
#include <unordered_map>
using namespace std::chrono_literals;
struct Parent:fsnet::ScriptHost {
 std::unordered_map<std::string,fsnet::Handler> handlers;
 bool has_route(const std::string& s)const override{return handlers.contains(s);}
 void bind(std::string s,fsnet::Handler h)override{handlers.emplace(std::move(s),std::move(h));}
 void unbind(const std::string&s)override{handlers.erase(s);}
 std::uint64_t connect(const std::string&,std::uint16_t)override{return 0;}
 void call(std::uint64_t,std::string,std::string,std::chrono::milliseconds,fsnet::Reply)override{}
};
int main(){
 auto parent=std::make_shared<Parent>();auto db=fsnet::memory_database();
 fsnet::EntityScriptHost host(parent,db,"owner/boot");host.enable_delivery(true);
 fsnet::EntitySchema schema;host.define_schema("Target",schema);
 auto initial=fsnet::encode_attributes({0,0,true,{}});
 host.bind_stateful("Target.reward","Target",initial,[](std::string input,fsnet::Reply reply){
  auto s=fsnet::parse_state_message(fsnet::parse_entity_message(input)->payload);
  reply({fsnet::Status::ok,fsnet::state_message(s->state,"reward applied")});
 });
 host.bind_stateful("Source.issue","Source","",[&](std::string input,fsnet::Reply reply){
  auto s=fsnet::parse_state_message(fsnet::parse_entity_message(input)->payload);
  host.send_entity("Source","alice","Target","bob","reward","",[s,reply](auto r){
   if(r.status==fsnet::Status::ok)reply({r.status,fsnet::state_message(s->state,r.data)});else reply(r);
  });
 });
 auto wait=[&](auto start){std::optional<fsnet::Result> r;start([&](auto value){r=std::move(value);});
  auto end=std::chrono::steady_clock::now()+2s;while(!r){host.poll();if(std::chrono::steady_clock::now()>end)throw std::runtime_error("timeout");std::this_thread::sleep_for(1ms);}return *r;};
 auto init=wait([&](auto cb){host.read_attributes("Target","bob",cb);});
 auto issue=wait([&](auto cb){parent->handlers.at("Source.issue")(fsnet::entity_message("alice",""),cb);});
 auto source=fsnet::decode_entity_record(db->get(fsnet::entity_record_key("Source","alice"))->value);
 auto receipt=wait([&](auto cb){host.receive_delivery({fsnet::Caller::server,"owner"},{"Source","alice",source.snapshot.delivery.epoch,source.snapshot.delivery.outbox.front()},cb);});
 auto [ticket,result]=fsnet::decode_delivery_ack(receipt.data);
 auto ack=wait([&](auto cb){host.acknowledge_delivery("Source","alice",source.snapshot.delivery.epoch,ticket,result,cb);});
 source=fsnet::decode_entity_record(db->get(fsnet::entity_record_key("Source","alice"))->value);
 std::cout<<"target_init="<<int(init.status)<<" source_issue="<<int(issue.status)<<" receive="<<int(receipt.status)<<" target_result="<<result.data<<" ack="<<int(ack.status)<<" pending="<<!source.snapshot.delivery.outbox.front().done<<"\n";
}

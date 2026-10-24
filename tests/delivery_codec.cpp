#include "fsnet/entity_delivery_state.hpp"
#include "fsnet/entity_snapshot.hpp"
#include <iostream>
#include <stdexcept>
#define CHECK(x) do{if(!(x))throw std::runtime_error(#x);}while(false)
template<class F> void rejects(F f){bool failed=false;try{f();}catch(...){failed=true;}CHECK(failed);}
int main(){try{
    fsnet::DeliveryEnvelope e;e.source_type="Quest";e.source_id="alice";e.source_epoch=123;
    e.message.type="Player";e.message.id="bob";e.message.method="grant";e.message.payload=std::string("a\0b\xff",4);e.message.target_epoch=456;e.message.sequence=1;e.message.ticket=fsnet::delivery_ticket(e);
    auto packet=fsnet::encode_delivery(e);auto decoded=fsnet::decode_delivery(packet);
    CHECK(decoded.message.payload==e.message.payload && decoded.message.ticket==e.message.ticket);
    for(std::size_t n=0;n<packet.size();++n)rejects([&]{fsnet::decode_delivery(std::string_view(packet).substr(0,n));});
    auto invalid=e;invalid.message.ticket[0]=invalid.message.ticket[0]=='a'?'b':'a';rejects([&]{fsnet::decode_delivery(fsnet::encode_delivery(invalid));});
    invalid=e;invalid.message.payload.resize(fsnet::delivery_payload_limit+1);
    rejects([&]{fsnet::encode_delivery(invalid);});
    rejects([&]{fsnet::decode_delivery_ack(std::string("FDA1")+std::string(fsnet::delivery_payload_limit+128,'x'));});
    fsnet::EntitySnapshot snapshot;snapshot.state=std::string("state\0bytes",11);snapshot.receipts.push_back({"old-id","route","input","output"});
    auto old=fsnet::encode_snapshot(snapshot);CHECK(old.starts_with("FSP1"));CHECK(fsnet::decode_snapshot(old).state==snapshot.state);
    snapshot.delivery.epoch=e.source_epoch;snapshot.delivery.outbox.push_back(e.message);
    auto channel=fsnet::delivery_channel(e.message.type,e.message.id,e.message.target_epoch);snapshot.delivery.sent[channel]=1;
    snapshot.delivery.inbox[fsnet::delivery_channel("Other","source",789)]={77,fsnet::delivery_digest(e),{fsnet::Status::rejected,"no"}};
    auto encoded=fsnet::encode_snapshot(snapshot);CHECK(encoded.starts_with("FSP2"));auto restored=fsnet::decode_snapshot(encoded);
    CHECK(restored.state==snapshot.state && restored.receipts.front().response=="output");
    CHECK(restored.delivery.outbox.front().payload==e.message.payload && restored.delivery.sent.at(channel)==1);
    CHECK(restored.delivery.inbox.begin()->second.sequence==77);
    CHECK(fsnet::delivery_charge(snapshot.delivery)>=fsnet::delivery_payload_limit);
    auto ack=fsnet::encode_delivery_ack(e.message.ticket,{fsnet::Status::ok,std::string("x\0y",3)});
    auto [ticket,result]=fsnet::decode_delivery_ack(ack);CHECK(ticket==e.message.ticket && result.data==std::string("x\0y",3));
    for(std::size_t n=0;n<ack.size();++n)rejects([&]{fsnet::decode_delivery_ack(std::string_view(ack).substr(0,n));});
    snapshot.delivery.outbox.push_back(e.message);rejects([&]{fsnet::encode_snapshot(snapshot);});snapshot.delivery.outbox.pop_back();
    snapshot.delivery.epoch=0;rejects([&]{fsnet::encode_snapshot(snapshot);});
    auto sources=std::vector<std::pair<std::string,std::string>>{{"Quest","alice"},{"Player","bob"}};
    CHECK(fsnet::decode_delivery_sources(fsnet::encode_delivery_sources(sources))==sources);
    sources.push_back(sources.front());rejects([&]{fsnet::decode_delivery_sources(fsnet::encode_delivery_sources(sources));});
    rejects([&]{fsnet::decode_delivery_state("FDS1");});rejects([&]{fsnet::delivery_channel("Quest","alice",0);});
    std::cout<<"PASS durable codecs: native binary values, FSP1/FSP2 compatibility, complete/truncated frames, ACK identity, duplicate tickets and bounded metadata\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}

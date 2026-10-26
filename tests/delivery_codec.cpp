#include "fsnet/entity_delivery_state.hpp"
#include "fsnet/entity_snapshot.hpp"
#include "fsnet/entity_id.hpp"
#include <iostream>
#include <stdexcept>
#define CHECK(x) do{if(!(x))throw std::runtime_error(#x);}while(false)
template<class F> void rejects(F f){bool failed=false;try{f();}catch(...){failed=true;}CHECK(failed);}
int main(){try{
    const auto source=fsnet::EntityId(1).str(),target=fsnet::EntityId(2).str(),other=fsnet::EntityId(3).str();
    fsnet::DeliveryEnvelope e;e.source_type="Quest";e.source_id=source;e.source_epoch=123;
    e.message.type="Player";e.message.id=target;e.message.method="grant";e.message.payload=std::string("a\0b\xff",4);e.message.target_epoch=456;e.message.sequence=1;e.message.ticket=fsnet::delivery_ticket(e);
    auto packet=fsnet::encode_delivery(e);auto decoded=fsnet::decode_delivery(packet);
    CHECK(decoded.message.payload==e.message.payload && decoded.message.ticket==e.message.ticket);
    for(std::size_t n=0;n<packet.size();++n)rejects([&]{fsnet::decode_delivery(std::string_view(packet).substr(0,n));});
    auto invalid=e;invalid.message.ticket[0]=invalid.message.ticket[0]=='a'?'b':'a';rejects([&]{fsnet::decode_delivery(fsnet::encode_delivery(invalid));});
    invalid=e;invalid.message.payload.resize(fsnet::delivery_payload_limit+1);
    rejects([&]{fsnet::encode_delivery(invalid);});
    rejects([&]{fsnet::decode_delivery_ack(std::string("FDA1")+std::string(fsnet::delivery_payload_limit+128,'x'));});
    fsnet::EntitySnapshot snapshot;snapshot.state=std::string("state\0bytes",11);snapshot.receipts.push_back({"request-1","route","input","output"});
    auto plain=fsnet::encode_snapshot(snapshot);CHECK(plain.starts_with("FSP1"));CHECK(fsnet::decode_snapshot(plain).state==snapshot.state);
    snapshot.delivery.epoch=e.source_epoch;snapshot.delivery.outbox.push_back(e.message);
    auto channel=fsnet::delivery_channel(e.message.type,e.message.id,e.message.target_epoch);snapshot.delivery.sent[channel]=1;
    snapshot.delivery.inbox[fsnet::delivery_channel("Other",other,789)]={77,fsnet::delivery_digest(e),{fsnet::Status::rejected,"no"}};
    auto encoded=fsnet::encode_snapshot(snapshot);CHECK(encoded.starts_with("FSP2"));auto restored=fsnet::decode_snapshot(encoded);
    CHECK(restored.state==snapshot.state && restored.receipts.front().response=="output");
    CHECK(restored.delivery.outbox.front().payload==e.message.payload && restored.delivery.sent.at(channel)==1);
    CHECK(restored.delivery.inbox.begin()->second.sequence==77);
    // FSP1/2/3 are current writer variants, selected by optional durable state.
    auto timed=snapshot;timed.timers.sequence=1;timed.timers.token="current-timers";
    timed.timers.entries.emplace("once",fsnet::EntityTimer{"once","grant",std::string("x\0y",3),"generation",50});
    auto timed_bytes=fsnet::encode_snapshot(timed);CHECK(timed_bytes.starts_with("FSP3"));
    auto restored_timed=fsnet::decode_snapshot(timed_bytes);
    CHECK(restored_timed.timers.entries.at("once").payload==std::string("x\0y",3));
    CHECK(restored_timed.delivery.outbox.front().ticket==e.message.ticket);
    timed.delivery={};timed_bytes=fsnet::encode_snapshot(timed);
    CHECK(fsnet::decode_snapshot(timed_bytes).delivery.epoch==0);
    CHECK(fsnet::decode_snapshot_timers(timed_bytes).entries.at("once").due_ms==50);
    for(const auto& bytes:{plain,encoded,timed_bytes}) {
        fsnet::validate_snapshot_framing(bytes);
        for(std::size_t n=0;n<bytes.size();++n) {
            rejects([&]{fsnet::decode_snapshot(std::string_view(bytes).substr(0,n));});
            rejects([&]{fsnet::validate_snapshot_framing(std::string_view(bytes).substr(0,n));});
        }
        rejects([&]{fsnet::decode_snapshot(bytes+"trailing");});
        rejects([&]{fsnet::validate_snapshot_framing(bytes+"trailing");});
    }
    for(auto unsupported:{"","raw-state","FSP0","FSP4"}) {
        rejects([&]{fsnet::decode_snapshot(unsupported);});
        rejects([&]{fsnet::validate_snapshot_framing(unsupported);});
    }
    CHECK(fsnet::delivery_charge(snapshot.delivery)>=fsnet::delivery_payload_limit);
    auto ack=fsnet::encode_delivery_ack(e.message.ticket,{fsnet::Status::ok,std::string("x\0y",3)});
    auto [ticket,result]=fsnet::decode_delivery_ack(ack);CHECK(ticket==e.message.ticket && result.data==std::string("x\0y",3));
    for(std::size_t n=0;n<ack.size();++n)rejects([&]{fsnet::decode_delivery_ack(std::string_view(ack).substr(0,n));});
    snapshot.delivery.outbox.push_back(e.message);rejects([&]{fsnet::encode_snapshot(snapshot);});snapshot.delivery.outbox.pop_back();
    snapshot.delivery.epoch=0;rejects([&]{fsnet::encode_snapshot(snapshot);});
    auto sources=std::vector<std::pair<std::string,std::string>>{{"Quest",source},{"Player",target}};
    CHECK(fsnet::decode_delivery_sources(fsnet::encode_delivery_sources(sources))==sources);
    sources.push_back(sources.front());rejects([&]{fsnet::decode_delivery_sources(fsnet::encode_delivery_sources(sources));});
    rejects([&]{fsnet::decode_delivery_state("FDS1");});rejects([&]{fsnet::delivery_channel("Quest",source,0);});
    std::cout<<"PASS durable codecs: native binary values, current FSP1/FSP2/FSP3 variants, complete/truncated frames, ACK identity, duplicate tickets and bounded metadata\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}

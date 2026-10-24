#include "fsnet/entity_delivery_state.hpp"
#include "fsnet/entity_record.hpp"
#include "fsnet/entity_message.hpp"
#include "fsnet/replication_wire.hpp"
#include <openssl/sha.h>
#include <set>
namespace fsnet {
namespace {
using snapshot_detail::blob;
using snapshot_detail::number;
using entity_record_detail::integer;
constexpr std::size_t max_state=8*1024*1024;
std::string hash(std::string_view input) {
    unsigned char digest[SHA256_DIGEST_LENGTH]; SHA256(reinterpret_cast<const unsigned char*>(input.data()),input.size(),digest);
    constexpr char hex[]="0123456789abcdef";std::string out;out.reserve(64);
    for(auto b:digest){out+=hex[b>>4];out+=hex[b&15];}return out;
}
void result(std::string& out,const Result& value) {
    if(static_cast<unsigned>(value.status)>static_cast<unsigned>(Status::internal)||value.data.size()>delivery_payload_limit)throw std::invalid_argument("invalid delivery result");
    out+=static_cast<char>(value.status);blob(out,value.data);
}
Result result(std::string_view& in) {
    if(in.empty()||static_cast<unsigned char>(in.front())>static_cast<unsigned>(Status::internal))throw std::invalid_argument("invalid delivery status");
    auto status=static_cast<Status>(in.front());in.remove_prefix(1);auto data=blob(in);
    if(data.size()>delivery_payload_limit)throw std::invalid_argument("delivery result too large");
    return {status,std::move(data)};
}
void message(std::string& out,const DeliveryMessage& m) {
    (void)entity_route(m.type,m.method);(void)encode_entity_key(m.type,m.id);
    if(m.method.front()=='$'||!m.target_epoch||!m.sequence||m.ticket.size()!=64||m.payload.size()>delivery_payload_limit)throw std::invalid_argument("invalid delivery message");
    blob(out,m.ticket);out+=encode_entity_key(m.type,m.id);blob(out,m.method);blob(out,m.payload);
    integer(out,m.target_epoch);integer(out,m.sequence);out+=m.done?1:0;result(out,m.result);
}
DeliveryMessage message(std::string_view& in) {
    DeliveryMessage m;m.ticket=blob(in);auto [type,id]=decode_entity_key(in);m.type=std::move(type);m.id=std::move(id);
    m.method=blob(in);m.payload=blob(in);m.target_epoch=integer(in);m.sequence=integer(in);
    if(in.empty()||static_cast<unsigned char>(in.front())>1)throw std::invalid_argument("invalid delivery phase");
    m.done=in.front()!=0;in.remove_prefix(1);m.result=result(in);
    std::string check;message(check,m);return m;
}
void end(std::string_view in){if(!in.empty())throw std::invalid_argument("trailing delivery bytes");}
void channel_key(std::string_view in) {
    (void)decode_entity_key(in);
    if(!integer(in))throw std::invalid_argument("missing channel epoch");
    end(in);
}
}
std::string delivery_channel(const std::string& type,const std::string& id,std::uint64_t epoch){if(!epoch)throw std::invalid_argument("missing channel epoch");auto out=encode_entity_key(type,id);integer(out,epoch);return out;}
std::string encode_delivery(const DeliveryEnvelope& e){
    if(!e.source_epoch||e.message.done)throw std::invalid_argument("invalid pending delivery");
    auto out=std::string("FSD1")+encode_entity_key(e.source_type,e.source_id);integer(out,e.source_epoch);message(out,e.message);return out;
}
DeliveryEnvelope decode_delivery(std::string_view in){
    if(!in.starts_with("FSD1")||in.size()>delivery_payload_limit*3)throw std::invalid_argument("invalid delivery envelope");
    in.remove_prefix(4);
    DeliveryEnvelope e;auto [type,id]=decode_entity_key(in);e.source_type=std::move(type);e.source_id=std::move(id);e.source_epoch=integer(in);e.message=message(in);end(in);
    if(!e.source_epoch||e.message.done||delivery_ticket(e)!=e.message.ticket)throw std::invalid_argument("invalid delivery ticket");
    return e;
}
std::string delivery_ticket(const DeliveryEnvelope& e){
    auto out=delivery_channel(e.source_type,e.source_id,e.source_epoch)+delivery_channel(e.message.type,e.message.id,e.message.target_epoch);integer(out,e.message.sequence);return hash(out);
}
std::string delivery_digest(const DeliveryEnvelope& e){return hash(encode_delivery(e));}
std::string encode_delivery_ack(const std::string& ticket,const Result& value){if(ticket.size()!=64)throw std::invalid_argument("invalid delivery ACK");std::string out="FDA1";blob(out,ticket);result(out,value);return out;}
std::pair<std::string,Result> decode_delivery_ack(std::string_view in){if(!in.starts_with("FDA1"))throw std::invalid_argument("invalid delivery ACK");in.remove_prefix(4);auto ticket=blob(in);auto value=result(in);end(in);if(ticket.size()!=64)throw std::invalid_argument("invalid ACK ticket");return {std::move(ticket),std::move(value)};}
std::string encode_delivery_state(const EntityDeliveryState& s){
    if(!s.epoch||s.sent.size()>4096||s.inbox.size()>4096||s.outbox.size()>1024)throw std::invalid_argument("invalid delivery state");
    std::string out="FDS1";integer(out,s.epoch);number(out,s.sent.size());
    for(const auto& [key,n]:s.sent){channel_key(key);if(!n)throw std::invalid_argument("missing sequence");blob(out,key);integer(out,n);}
    number(out,s.inbox.size());for(const auto& [key,c]:s.inbox){channel_key(key);if(!c.sequence || c.digest.size()!=64)throw std::invalid_argument("invalid inbox cursor");blob(out,key);integer(out,c.sequence);blob(out,c.digest);result(out,c.result);}
    number(out,s.outbox.size());std::set<std::string> tickets;
    for(const auto& m:s.outbox){if(!tickets.insert(m.ticket).second)throw std::invalid_argument("duplicate delivery ticket");message(out,m);}
    if(out.size()>max_state)throw std::length_error("delivery state too large");
    return out;
}
EntityDeliveryState decode_delivery_state(std::string_view in){
    if(!in.starts_with("FDS1")||in.size()>max_state)throw std::invalid_argument("invalid delivery state");
    in.remove_prefix(4);
    EntityDeliveryState s;s.epoch=integer(in);auto n=number(in);if(n>4096)throw std::invalid_argument("delivery counter limit");
    for(unsigned i=0;i<n;++i){auto key=blob(in);channel_key(key);auto seq=integer(in);if(key.size()>512||!seq||!s.sent.emplace(key,seq).second)throw std::invalid_argument("invalid delivery counter");}
    n=number(in);if(n>4096)throw std::invalid_argument("delivery inbox limit");
    for(unsigned i=0;i<n;++i){auto key=blob(in);channel_key(key);DeliveryCursor c;c.sequence=integer(in);c.digest=blob(in);c.result=result(in);if(key.size()>512||!c.sequence||c.digest.size()!=64||!s.inbox.emplace(key,std::move(c)).second)throw std::invalid_argument("invalid delivery cursor");}
    n=number(in);if(n>1024)throw std::invalid_argument("delivery outbox limit");std::set<std::string> tickets;
    for(unsigned i=0;i<n;++i){auto m=message(in);if(!tickets.insert(m.ticket).second)throw std::invalid_argument("duplicate delivery ticket");s.outbox.push_back(std::move(m));}
    end(in);if(!s.epoch)throw std::invalid_argument("missing delivery epoch");return s;
}
std::size_t delivery_charge(const EntityDeliveryState& s){
    if(!s.epoch)return 0;
    auto bytes=encode_delivery_state(s).size();
    for(const auto& m:s.outbox)if(!m.done)bytes+=delivery_payload_limit;
    return bytes;
}
std::string encode_delivery_sources(const std::vector<std::pair<std::string,std::string>>& sources){
    if(sources.size()>65536)throw std::length_error("delivery registry full");
    std::string out="FDR1";number(out,sources.size());
    for(const auto& [type,id]:sources)out+=encode_entity_key(type,id);
    return out;
}
std::vector<std::pair<std::string,std::string>> decode_delivery_sources(std::string_view in){
    if(!in.starts_with("FDR1")||in.size()>32*1024*1024)throw std::invalid_argument("invalid delivery registry");
    in.remove_prefix(4);auto n=number(in);if(n>65536)throw std::invalid_argument("delivery registry limit");
    std::vector<std::pair<std::string,std::string>> out;std::set<std::pair<std::string,std::string>> seen;
    for(unsigned i=0;i<n;++i){auto key=decode_entity_key(in);if(!seen.insert(key).second)throw std::invalid_argument("duplicate delivery source");out.push_back(std::move(key));}end(in);return out;
}
}

#pragma once
#include "fsnet/node.hpp"
#include <map>
#include <vector>
namespace fsnet {
inline constexpr std::size_t delivery_payload_limit=65536;
struct DeliveryMessage {
    std::string ticket,type,id,method,payload;
    std::uint64_t target_epoch=0,sequence=0;
    bool done=false;
    Result result{Status::ok,{}};
};
struct DeliveryCursor {
    std::uint64_t sequence=0;
    std::string digest;
    Result result{Status::ok,{}};
};
struct EntityDeliveryState {
    std::uint64_t epoch=0;
    std::map<std::string,std::uint64_t> sent;
    std::map<std::string,DeliveryCursor> inbox;
    std::vector<DeliveryMessage> outbox;
};
struct DeliveryEnvelope {
    std::string source_type,source_id;
    std::uint64_t source_epoch=0;
    DeliveryMessage message;
};
std::string encode_delivery_state(const EntityDeliveryState&);
EntityDeliveryState decode_delivery_state(std::string_view);
std::string encode_delivery(const DeliveryEnvelope&);
DeliveryEnvelope decode_delivery(std::string_view);
std::string delivery_channel(const std::string& type,const std::string& id,std::uint64_t epoch);
std::string delivery_ticket(const DeliveryEnvelope&);
std::string delivery_digest(const DeliveryEnvelope&);
std::string encode_delivery_ack(const std::string& ticket,const Result&);
std::pair<std::string,Result> decode_delivery_ack(std::string_view);
// Includes reserved result capacity for accepted pending messages.
std::size_t delivery_charge(const EntityDeliveryState&);
inline constexpr const char* delivery_registry_key="fsnet/delivery/sources";
std::vector<std::pair<std::string,std::string>> decode_delivery_sources(std::string_view);
std::string encode_delivery_sources(const std::vector<std::pair<std::string,std::string>>&);
}

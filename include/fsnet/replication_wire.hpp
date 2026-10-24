#pragma once
#include "fsnet/node.hpp"
#include "fsnet/attribute_wire.hpp"
#include <optional>
namespace fsnet {
struct ReplicationPacket {
    std::uint64_t subscription;
    std::string type,id;
    Status status;
    std::uint32_t lease_ms;
    std::optional<AttributeUpdate> update;
};
std::string encode_entity_key(const std::string& type,const std::string& id);
std::pair<std::string,std::string> decode_entity_key(std::string_view&);
std::string encode_replication_packet(std::uint64_t subscription,const std::string& type,const std::string& id,
                                     Status,const AttributeUpdate* = nullptr,std::uint32_t lease_ms = 0);
ReplicationPacket decode_replication_packet(std::string_view);
}

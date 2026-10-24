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
struct ReplicationUpdateHeader { std::uint64_t base,revision; bool snapshot; };
// Borrows type/id from input. Attribute bytes undergo the same full validation
// as the owning decoder; only the forwarding metadata survives the parse.
struct ReplicationPacketView {
    std::uint64_t subscription;
    std::string_view type,id;
    Status status;
    std::uint32_t lease_ms;
    std::optional<ReplicationUpdateHeader> update;
};
ReplicationPacketView decode_replication_packet_view(std::string_view);
// Rewrites only the eight-byte ID. Forwarders validate the full packet first.
void remap_replication_subscription(std::string&,std::uint64_t,std::size_t prefix_bytes=0);
std::string encode_entity_key(const std::string& type,const std::string& id);
std::pair<std::string,std::string> decode_entity_key(std::string_view&);
std::string encode_replication_packet(std::uint64_t subscription,const std::string& type,const std::string& id,
                                     Status,const AttributeUpdate* = nullptr,std::uint32_t lease_ms = 0);
ReplicationPacket decode_replication_packet(std::string_view);
}

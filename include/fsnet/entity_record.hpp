#pragma once
#include "fsnet/entity_snapshot.hpp"
#include <limits>

namespace fsnet {
// One atomic record contains every field needed to validate and publish a state
// transition. Storage never needs to coordinate a lease key with a state key.
struct EntityRecord {
    std::string owner;
    std::int64_t expires_ms = 0;
    std::uint64_t fence = 0;
    EntitySnapshot snapshot;
};
namespace entity_record_detail {
inline void integer(std::string& out, std::uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8) out.push_back(static_cast<char>(value >> shift));
}
inline std::uint64_t integer(std::string_view& input) {
    if (input.size() < 8) throw std::runtime_error("truncated entity record");
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i) value = (value << 8) | static_cast<unsigned char>(input[i]);
    input.remove_prefix(8); return value;
}
}
inline std::string entity_record_key(const std::string& type, const std::string& id) {
    return "fsnet/entity/" + type + '\0' + id;
}
inline std::string encode_entity_record(const EntityRecord& record) {
    if (record.owner.size() > 256 || record.expires_ms < 0 || record.fence >= static_cast<std::uint64_t>(INT64_MAX))
        throw std::invalid_argument("invalid entity record");
    std::string result = "FER1";
    snapshot_detail::blob(result, record.owner);
    entity_record_detail::integer(result, static_cast<std::uint64_t>(record.expires_ms));
    entity_record_detail::integer(result, record.fence);
    result += encode_snapshot(record.snapshot); return result;
}
inline EntityRecord decode_entity_record(std::string_view input) {
    if (!input.starts_with("FER1")) throw std::runtime_error("unknown entity record version");
    input.remove_prefix(4); EntityRecord record; record.owner = snapshot_detail::blob(input);
    auto expires = entity_record_detail::integer(input); record.fence = entity_record_detail::integer(input);
    if (record.owner.size() > 256 || expires > static_cast<std::uint64_t>(INT64_MAX) || record.fence >= static_cast<std::uint64_t>(INT64_MAX))
        throw std::runtime_error("invalid entity record metadata");
    record.expires_ms = static_cast<std::int64_t>(expires); record.snapshot = decode_snapshot(input); return record;
}
}

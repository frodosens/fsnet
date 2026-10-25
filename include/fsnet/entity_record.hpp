#pragma once
#include "fsnet/entity_snapshot.hpp"
#include "fsnet/entity_id.hpp"
#include <limits>
#include <optional>

namespace fsnet {
struct EntityMetadata {
    std::string type, owner_account, creation_digest;
    bool rebuildable=false, destroyed=false;
    std::string initial_state;
};
struct EntityMetadataView {
    std::string_view type, owner_account, creation_digest;
    bool rebuildable=false, destroyed=false;
    std::string_view initial_state;
};
// One atomic record contains every field needed to validate and publish a state
// transition. Storage never needs to coordinate a lease key with a state key.
struct EntityRecord {
    std::string owner;
    std::int64_t expires_ms = 0;
    std::uint64_t fence = 0;
    EntitySnapshot snapshot;
    bool initialized = true; // FER1 records are already initialized
    std::string placement; // stable node; persists across release/restart
    std::uint64_t transfer_id = 0; // source storage version, never reused after delete/recreate
    std::optional<EntityMetadata> metadata = std::nullopt;
};
// Non-owning metadata and snapshot envelope. Views remain valid only while the
// encoded record's backing storage is unchanged and alive.
struct EntityRecordView {
    std::string_view owner;
    std::int64_t expires_ms = 0;
    std::uint64_t fence = 0;
    std::string_view snapshot;
    bool initialized = true;
    std::string_view placement;
    std::uint64_t transfer_id = 0;
    std::optional<EntityMetadataView> metadata = std::nullopt;
};
inline bool valid_entity_node(std::string_view node) {
    return !node.empty() && node.size() <= 127 && node.find_first_of("/\0",0,2)==std::string_view::npos;
}
inline bool entity_node_matches(std::string_view owner,std::string_view node) {
    return owner==node || (owner.size()>node.size() && owner.starts_with(node) && owner[node.size()]=='/');
}
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
    if(valid_entity_id(id)) return "fsnet/entity/v2/"+id;
    return "fsnet/entity/" + type + '\0' + id;
}
inline std::string entity_record_key(EntityId id) { return "fsnet/entity/v2/"+id.str(); }
template<class Metadata> inline bool valid_entity_metadata(const Metadata& metadata) {
    return !metadata.type.empty() && metadata.type.size()<=127 &&
        metadata.type.find_first_of(".\0",0,2)==std::string_view::npos && metadata.owner_account.size()<=256 &&
        metadata.creation_digest.size()==32 && metadata.initial_state.size()<=8*1024*1024 &&
        (metadata.rebuildable || metadata.initial_state.empty());
}
inline std::string encode_entity_record(const EntityRecord& record) {
    if (record.owner.size() > 256 || record.expires_ms < 0 || record.fence >= static_cast<std::uint64_t>(INT64_MAX))
        throw std::invalid_argument("invalid entity record");
    if ((!record.placement.empty() && (!valid_entity_node(record.placement) || (!record.transfer_id && !record.metadata))) ||
        (record.placement.empty() && record.transfer_id)) throw std::invalid_argument("invalid entity placement");
    if(record.metadata && !valid_entity_metadata(*record.metadata)) throw std::invalid_argument("invalid global entity metadata");
    std::string result = record.metadata ? "FER4" : (!record.placement.empty() ? "FER3" : (record.initialized ? "FER1" : "FER2"));
    snapshot_detail::blob(result, record.owner);
    entity_record_detail::integer(result, static_cast<std::uint64_t>(record.expires_ms));
    entity_record_detail::integer(result, record.fence);
    if (record.metadata || !record.placement.empty()) {
        result.push_back(record.initialized ? 1 : 0); snapshot_detail::blob(result,record.placement);
        entity_record_detail::integer(result,record.transfer_id);
    }
    if(record.metadata) {
        const auto& metadata=*record.metadata;
        snapshot_detail::blob(result,metadata.type); snapshot_detail::blob(result,metadata.owner_account);
        result.push_back(static_cast<char>((metadata.rebuildable?1:0)|(metadata.destroyed?2:0)));
        snapshot_detail::blob(result,metadata.initial_state); result+=metadata.creation_digest;
    }
    result += encode_snapshot(record.snapshot); return result;
}
namespace entity_record_detail {
inline EntityRecordView header(std::string_view input) {
    bool global=input.starts_with("FER4"), placed=global || input.starts_with("FER3"), pending=input.starts_with("FER2");
    if (!placed && !pending && !input.starts_with("FER1")) throw std::runtime_error("unknown entity record version");
    input.remove_prefix(4); EntityRecordView record; record.initialized=!pending; record.owner = snapshot_detail::blob_view(input);
    auto expires = entity_record_detail::integer(input); record.fence = entity_record_detail::integer(input);
    if (record.owner.size() > 256 || expires > static_cast<std::uint64_t>(INT64_MAX) || record.fence >= static_cast<std::uint64_t>(INT64_MAX))
        throw std::runtime_error("invalid entity record metadata");
    if (placed) {
        if(input.empty() || static_cast<unsigned char>(input.front())>1) throw std::runtime_error("invalid entity initialization");
        record.initialized=input.front()!=0; input.remove_prefix(1); record.placement=snapshot_detail::blob_view(input);
        record.transfer_id=entity_record_detail::integer(input);
        if((!record.placement.empty() && !valid_entity_node(record.placement)) ||
           (!global && (!record.transfer_id || record.placement.empty())) ||
           (record.placement.empty() && record.transfer_id)) throw std::runtime_error("invalid entity placement");
    }
    if(global) {
        EntityMetadataView metadata; metadata.type=snapshot_detail::blob_view(input); metadata.owner_account=snapshot_detail::blob_view(input);
        if(input.empty() || static_cast<unsigned char>(input.front())>3) throw std::runtime_error("invalid global entity flags");
        metadata.rebuildable=(input.front()&1)!=0; metadata.destroyed=(input.front()&2)!=0; input.remove_prefix(1);
        metadata.initial_state=snapshot_detail::blob_view(input);
        if(input.size()<32) throw std::runtime_error("truncated creation digest");
        metadata.creation_digest=input.substr(0,32); input.remove_prefix(32);
        if(!valid_entity_metadata(metadata)) throw std::runtime_error("invalid global entity metadata");
        record.metadata=metadata;
    }
    record.expires_ms = static_cast<std::int64_t>(expires); record.snapshot = input; return record;
}
}
inline EntityRecordView decode_entity_record_view(std::string_view input) {
    auto record = entity_record_detail::header(input);
    validate_snapshot_framing(record.snapshot);
    return record;
}
inline EntityRecord decode_entity_record(std::string_view input) {
    auto view = entity_record_detail::header(input);
    EntityRecord record;
    record.owner = view.owner; record.expires_ms = view.expires_ms; record.fence = view.fence;
    record.initialized = view.initialized; record.placement = view.placement; record.transfer_id = view.transfer_id;
    if(view.metadata) {
        const auto& metadata=*view.metadata;
        record.metadata=EntityMetadata{std::string(metadata.type),std::string(metadata.owner_account),std::string(metadata.creation_digest),
            metadata.rebuildable,metadata.destroyed,std::string(metadata.initial_state)};
    }
    record.snapshot = decode_snapshot(view.snapshot); return record;
}
}

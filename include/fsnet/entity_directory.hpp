#pragma once
#include "fsnet/database.hpp"
#include "fsnet/entity_record.hpp"
#include <stdexcept>

namespace fsnet {
struct EntityLocation { std::string owner; std::uint64_t fence; std::int64_t expires_ms; std::string placement{}; std::uint64_t transfer_id=0; std::string type{}; };
inline std::optional<EntityLocation> entity_location(Database& database, const std::string& type, const std::string& id) {
    auto record = database.get(entity_record_key(type, id));
    if (!record) return std::nullopt;
    auto value = decode_entity_record_view(record->value);
    if(value.metadata && (value.metadata->destroyed || (!type.empty() && value.metadata->type!=type)))return {};
    if(valid_entity_id(id) && !value.metadata)return {};
    return EntityLocation{std::string(value.owner), value.fence, value.expires_ms, std::string(value.placement), value.transfer_id,
        value.metadata?std::string(value.metadata->type):type};
}
inline std::optional<EntityLocation> entity_location(Database& database,EntityId id) {return entity_location(database,{},id.str());}
}

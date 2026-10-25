#pragma once
#include "fsnet/database.hpp"
#include "fsnet/entity_record.hpp"
#include <stdexcept>

namespace fsnet {
struct EntityLocation { std::string owner; std::uint64_t fence; std::int64_t expires_ms; std::string placement{}; std::uint64_t transfer_id=0; std::string type{}; };
inline std::optional<EntityLocation> entity_location(Storage& database, const std::string& type, const std::string& id) {
    if(!valid_entity_id(id))return {};
    auto record = database.get(entity_record_key(EntityId(id)));
    if (!record) return std::nullopt;
    auto value = decode_entity_record_view(record->value);
    if(!value.metadata || value.metadata->destroyed || (!type.empty() && value.metadata->type!=type))return {};
    return EntityLocation{std::string(value.owner), value.fence, value.expires_ms, std::string(value.placement), value.transfer_id,
        std::string(value.metadata->type)};
}
inline std::optional<EntityLocation> entity_location(Storage& database,EntityId id) {return entity_location(database,{},id.str());}
}

#pragma once
#include "fsnet/database.hpp"
#include "fsnet/entity_record.hpp"
#include <stdexcept>

namespace fsnet {
struct EntityLocation { std::string owner; std::uint64_t fence; std::int64_t expires_ms; std::string placement{}; std::uint64_t transfer_id=0; };
inline std::optional<EntityLocation> entity_location(Database& database, const std::string& type, const std::string& id) {
    auto record = database.get(entity_record_key(type, id));
    if (!record) return std::nullopt;
    auto value = decode_entity_record(record->value);
    return EntityLocation{std::move(value.owner), value.fence, value.expires_ms, std::move(value.placement), value.transfer_id};
}
}

#pragma once
#include "fsnet/database.hpp"
#include <stdexcept>

namespace fsnet {
struct EntityLocation { std::string owner; std::uint64_t fence; std::int64_t expires_ms; };
inline std::optional<EntityLocation> entity_location(Database& database, const std::string& type, const std::string& id) {
    auto record = database.get("fsnet/actor/owner/" + type + '\0' + id);
    if (!record) return std::nullopt;
    auto split = record->value.find(':');
    if (split == std::string::npos || !split) throw std::runtime_error("corrupt entity directory lease");
    std::size_t end = 0; auto expires = std::stoll(record->value.substr(0, split), &end);
    if (end != split || expires < 0) throw std::runtime_error("corrupt entity directory expiry");
    return EntityLocation{record->value.substr(split + 1), record->version, expires};
}
}

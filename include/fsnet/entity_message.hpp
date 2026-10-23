#pragma once
#include <stdexcept>
#include <string>
#include <string_view>

namespace fsnet {
// Entity RPC wire envelope: FSE1, big-endian uint16 ID length, ID, opaque payload.
inline std::string entity_message(std::string_view id, std::string_view payload) {
    if (id.empty() || id.size() > 256) throw std::invalid_argument("invalid entity ID");
    std::string result = "FSE1";
    result.push_back(static_cast<char>(id.size() >> 8));
    result.push_back(static_cast<char>(id.size() & 255));
    result.append(id); result.append(payload);
    return result;
}
}

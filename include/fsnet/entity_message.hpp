#pragma once
#include <stdexcept>
#include <string>
#include <string_view>
#include <optional>
#include <cstdint>

namespace fsnet {
// Entity RPC wire envelope: FSE1, big-endian uint16 ID length, ID, opaque payload.
inline std::string entity_message(std::string_view id, std::string_view payload, std::string_view request_id = {}) {
    if (id.empty() || id.size() > 256) throw std::invalid_argument("invalid entity ID");
    if (request_id.size() > 128) throw std::invalid_argument("request ID too large");
    std::string result = request_id.empty() ? "FSE1" : "FSE2";
    result.push_back(static_cast<char>(id.size() >> 8));
    result.push_back(static_cast<char>(id.size() & 255));
    result.append(id);
    if (!request_id.empty()) { result.push_back(static_cast<char>(request_id.size() >> 8)); result.push_back(static_cast<char>(request_id.size())); result.append(request_id); }
    result.append(payload);
    return result;
}
struct EntityMessage { std::string id, payload, request_id; };
inline std::optional<EntityMessage> parse_entity_message(std::string_view input) {
    if (input.size() < 6 || (input.substr(0, 4) != "FSE1" && input.substr(0, 4) != "FSE2")) return std::nullopt;
    auto length = (static_cast<unsigned char>(input[4]) << 8) | static_cast<unsigned char>(input[5]);
    if (!length || length > 256 || input.size() < 6u + length) return std::nullopt;
    auto payload = input.substr(6 + length); std::string token;
    if (input.substr(0, 4) == "FSE2") {
        if (payload.size() < 2) return std::nullopt;
        auto size = (static_cast<unsigned char>(payload[0]) << 8) | static_cast<unsigned char>(payload[1]);
        if (!size || size > 128 || payload.size() < 2u + size) return std::nullopt;
        token = payload.substr(2, size); payload.remove_prefix(2 + size);
    }
    return EntityMessage{std::string(input.substr(6, length)), std::string(payload), std::move(token)};
}
// Internal state-machine boundary, never accepted as client-supplied state.
inline std::string state_message(std::string_view state, std::string_view payload) {
    if (state.size() > UINT32_MAX) throw std::length_error("state too large");
    std::string result = "FSS1";
    for (int shift = 24; shift >= 0; shift -= 8) result.push_back(static_cast<char>(state.size() >> shift));
    result.append(state); result.append(payload); return result;
}
struct StateMessage { std::string state, payload; };
inline std::optional<StateMessage> parse_state_message(std::string_view input) {
    if (input.size() < 8 || input.substr(0, 4) != "FSS1") return std::nullopt;
    std::uint32_t length = 0;
    for (int i = 4; i < 8; ++i) length = (length << 8) | static_cast<unsigned char>(input[i]);
    if (length > input.size() - 8) return std::nullopt;
    return StateMessage{std::string(input.substr(8, length)), std::string(input.substr(8 + length))};
}
}

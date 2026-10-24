#pragma once
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include "fsnet/entity_delivery_state.hpp"

namespace fsnet {
struct EntityReceipt {
    std::string id, route, request, response;
    std::size_t bytes() const { return 16 + id.size() + route.size() + request.size() + response.size(); }
};
struct EntitySnapshot { std::string state; std::vector<EntityReceipt> receipts; EntityDeliveryState delivery{}; };
namespace snapshot_detail {
inline void number(std::string& out, std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) out.push_back(static_cast<char>(value >> shift));
}
inline void blob(std::string& out, const std::string& value) {
    if (value.size() > UINT32_MAX) throw std::length_error("snapshot field too large");
    number(out, static_cast<std::uint32_t>(value.size())); out += value;
}
inline std::uint32_t number(std::string_view& input) {
    if (input.size() < 4) throw std::runtime_error("truncated entity snapshot");
    std::uint32_t result = 0;
    for (int i = 0; i < 4; ++i) result = (result << 8) | static_cast<unsigned char>(input[i]);
    input.remove_prefix(4); return result;
}
inline std::string blob(std::string_view& input) {
    auto size = number(input);
    if (size > input.size()) throw std::runtime_error("truncated entity snapshot field");
    std::string result(input.substr(0, size)); input.remove_prefix(size); return result;
}
}
inline std::string encode_snapshot(const EntitySnapshot& snapshot) {
    if(!snapshot.delivery.epoch && (!snapshot.delivery.sent.empty() || !snapshot.delivery.inbox.empty() || !snapshot.delivery.outbox.empty()))
        throw std::invalid_argument("delivery metadata requires an epoch");
    std::string result = snapshot.delivery.epoch ? "FSP2" : "FSP1"; snapshot_detail::blob(result, snapshot.state);
    if (snapshot.receipts.size() > 1024) throw std::length_error("too many entity receipts");
    snapshot_detail::number(result, static_cast<std::uint32_t>(snapshot.receipts.size()));
    for (const auto& receipt : snapshot.receipts) {
        snapshot_detail::blob(result, receipt.id); snapshot_detail::blob(result, receipt.route);
        snapshot_detail::blob(result, receipt.request); snapshot_detail::blob(result, receipt.response);
    }
    if(snapshot.delivery.epoch) snapshot_detail::blob(result,encode_delivery_state(snapshot.delivery));
    return result;
}
inline EntitySnapshot decode_snapshot(std::string_view input) {
    bool delivery=input.starts_with("FSP2");
    if (!delivery && !input.starts_with("FSP1")) throw std::runtime_error("unknown entity snapshot version");
    input.remove_prefix(4); EntitySnapshot result; result.state = snapshot_detail::blob(input);
    auto count = snapshot_detail::number(input);
    if (count > 1024) throw std::runtime_error("invalid entity receipt count");
    for (std::uint32_t i = 0; i < count; ++i) result.receipts.push_back({snapshot_detail::blob(input),
        snapshot_detail::blob(input), snapshot_detail::blob(input), snapshot_detail::blob(input)});
    if(delivery) result.delivery=decode_delivery_state(snapshot_detail::blob(input));
    if (!input.empty()) throw std::runtime_error("trailing entity snapshot bytes");
    return result;
}
}

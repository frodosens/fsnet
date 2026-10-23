#pragma once
#include "fsnet/storage.hpp"
#include "fsnet/entity_record.hpp"

namespace fsnet {
// Offline only: all processes using the former two-key protocol must be stopped.
// Legacy keys are retained so operators can inspect/roll back the conversion.
inline bool migrate_legacy_entity(Storage& storage, const std::string& type, const std::string& id,
                                  std::int64_t now_ms, bool raw_state = false) {
    const auto target = entity_record_key(type, id), entity = type + '\0' + id;
    if (auto current = storage.get(target)) { decode_entity_record(current->value); return false; }
    const auto owner_key = "fsnet/actor/owner/" + entity, state_key = "fsnet/actor/state/" + entity;
    auto owner = storage.get(owner_key), state = storage.get(state_key);
    if (!owner && !state) throw std::runtime_error("no legacy entity found");
    if (owner) {
        auto split = owner->value.find(':'); std::size_t end = 0;
        if (split == std::string::npos || !split) throw std::runtime_error("invalid legacy lease");
        auto expiry = std::stoll(owner->value.substr(0, split), &end);
        if (end != split || expiry < 0) throw std::runtime_error("invalid legacy expiry");
        if (expiry > now_ms) throw std::runtime_error("legacy lease still active; stop old instances and wait for expiry");
    }
    EntityRecord record; record.fence = owner ? owner->version : 0;
    if (state) record.snapshot = !raw_state && state->value.starts_with("FSP1") ? decode_snapshot(state->value) : EntitySnapshot{state->value, {}};
    auto owner_after = storage.get(owner_key), state_after = storage.get(state_key);
    auto same = [](const std::optional<Record>& a, const std::optional<Record>& b) {
        return a.has_value() == b.has_value() && (!a || a->version == b->version);
    };
    if (!same(owner, owner_after) || !same(state, state_after)) throw std::runtime_error("legacy entity changed; stop every old writer before migration");
    if (!storage.compare_exchange(target, 0, encode_entity_record(record))) throw std::runtime_error("migration target changed concurrently");
    return true;
}
}

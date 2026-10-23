#pragma once
#include "fsnet/database.hpp"
#include "fsnet/entity_record.hpp"

namespace fsnet {
// Low-level explicit ownership helper. Lease/state/fence metadata is kept in one
// record, so even a filesystem-only backend needs no multi-key transaction.
class EntityStore {
    struct Value { std::string owner, state; std::uint64_t fence = 0, revision = 0; };
    std::shared_ptr<Storage> storage_;
    static std::string key(const std::string& entity) { return "fsnet/owned/" + entity; }
    static std::string encode(const Value& value) {
        std::string out = "FSO1"; snapshot_detail::blob(out, value.owner);
        entity_record_detail::integer(out, value.fence); entity_record_detail::integer(out, value.revision);
        snapshot_detail::blob(out, value.state); return out;
    }
    static Value decode(std::string_view input) {
        if (!input.starts_with("FSO1")) throw std::runtime_error("unknown owned entity record");
        input.remove_prefix(4); Value value; value.owner = snapshot_detail::blob(input);
        value.fence = entity_record_detail::integer(input); value.revision = entity_record_detail::integer(input);
        value.state = snapshot_detail::blob(input);
        if (!input.empty() || !value.fence) throw std::runtime_error("invalid owned entity record");
        return value;
    }
public:
    struct Ownership { std::string owner; std::uint64_t fence; };
    explicit EntityStore(std::shared_ptr<Storage> storage) : storage_(std::move(storage)) {
        if (!storage_) throw std::invalid_argument("storage required");
    }
    std::optional<Ownership> owner(const std::string& entity) {
        auto record = storage_->get(key(entity)); if (!record) return std::nullopt;
        auto value = decode(record->value); return Ownership{std::move(value.owner), value.fence};
    }
    bool transfer(const std::string& entity, std::uint64_t expected_fence, const std::string& new_owner) {
        if (entity.empty() || new_owner.empty() || expected_fence >= static_cast<std::uint64_t>(INT64_MAX) - 1)
            throw std::invalid_argument("invalid entity ownership");
        for (int attempt = 0; attempt < 8; ++attempt) {
            auto record = storage_->get(key(entity));
            if (!record && (storage_->get("fsnet/owner/" + entity) || storage_->get("fsnet/state/" + entity)))
                throw std::runtime_error("legacy owned entity needs offline conversion");
            auto value = record ? decode(record->value) : Value{};
            if (value.fence != expected_fence) return false;
            value.owner = new_owner; ++value.fence;
            if (storage_->compare_exchange(key(entity), record ? record->version : 0, encode(value))) return true;
        }
        return false;
    }
    std::optional<Record> load(const std::string& entity) {
        auto record = storage_->get(key(entity)); if (!record) return std::nullopt;
        auto value = decode(record->value);
        return value.revision ? std::optional<Record>{{std::move(value.state), value.revision}} : std::nullopt;
    }
    bool save(const std::string& entity, std::uint64_t fence, std::uint64_t expected_version, std::string state) {
        if (!fence || expected_version >= static_cast<std::uint64_t>(INT64_MAX) - 1) return false;
        auto record = storage_->get(key(entity)); if (!record) return false;
        auto value = decode(record->value);
        if (value.fence != fence || value.revision != expected_version) return false;
        value.state = std::move(state); ++value.revision;
        return storage_->compare_exchange(key(entity), record->version, encode(value));
    }
};
}

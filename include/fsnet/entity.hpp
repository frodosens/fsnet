#pragma once
#include "fsnet/database.hpp"
#include <stdexcept>

namespace fsnet {
// Ownership is granted by a linearizable Database CAS, not a health lease.
// An orchestrator chooses when to transfer ownership. Old owners may continue
// running after a partition, but their writes fail the atomic fence condition.
class EntityStore {
    std::shared_ptr<Database> database_;
    static std::string owner_key(const std::string& entity) { return "fsnet/owner/" + entity; }
    static std::string state_key(const std::string& entity) { return "fsnet/state/" + entity; }
public:
    struct Ownership { std::string owner; std::uint64_t fence; };
    explicit EntityStore(std::shared_ptr<Database> database) : database_(std::move(database)) {
        if (!database_) throw std::invalid_argument("database required");
    }
    std::optional<Ownership> owner(const std::string& entity) {
        auto record = database_->get(owner_key(entity));
        if (!record) return std::nullopt;
        return Ownership{record->value, record->version};
    }
    // expected_fence=0 for first ownership. On takeover pass the observed fence;
    // concurrent transfers cannot both succeed. Use globally unique owner IDs.
    bool transfer(const std::string& entity, std::uint64_t expected_fence, const std::string& new_owner) {
        if (entity.empty() || new_owner.empty()) throw std::invalid_argument("entity and owner required");
        return database_->compare_exchange(owner_key(entity), expected_fence, new_owner);
    }
    std::optional<Record> load(const std::string& entity) { return database_->get(state_key(entity)); }
    bool save(const std::string& entity, std::uint64_t fence, std::uint64_t expected_version,
              std::string value) {
        if (!fence) return false;
        return database_->transact({{owner_key(entity), fence, std::nullopt, true},
                                    {state_key(entity), expected_version, std::move(value)}});
    }
};
}

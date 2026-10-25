#pragma once
#include "fsnet/entity_id.hpp"
#include "fsnet/entity_record.hpp"
#include "fsnet/storage.hpp"
#include <mutex>

namespace fsnet {
// The catalog is synchronous: run it on the existing storage executor, never on
// the Node/VM thread. All allocators in a cluster share the same authoritative
// Storage namespace. Destruction never deletes identity/creation records.
class EntityIdAllocator {
    Storage& storage_;
    std::uint64_t block_size_, next_=0, remaining_=0;
    std::mutex mutex_;
public:
    explicit EntityIdAllocator(Storage&,std::uint64_t block_size=4096);
    EntityId allocate();
};
enum class EntityPersistence : unsigned char { durable=0, rebuildable=1 };
struct EntityCreation {
    std::string type, owner_account, state, target_node;
    EntityPersistence persistence=EntityPersistence::durable;
};
struct EntitySpawnOptions {
    std::string scope, creation_key, owner_account_id, target_node;
    EntityPersistence persistence=EntityPersistence::durable;
};
struct EntityDescription {
    EntityId id;
    EntityRecord record;
    std::uint64_t version;
};
class EntityCreationConflict : public std::runtime_error {
public:
    EntityCreationConflict() : std::runtime_error("entity creation key reused with different parameters") {}
};
class EntityCatalog {
    Storage& storage_;
    EntityIdAllocator allocator_;
public:
    explicit EntityCatalog(Storage& storage,std::uint64_t block_size=4096) : storage_(storage),allocator_(storage,block_size) {}
    // scope is the trusted creating entity ID or a configured bootstrap scope.
    // key must be stable across retries. The operation descriptor is permanent,
    // independent of bounded RPC receipts; lost ACKs cannot create a second ID.
    EntityId create(std::string_view scope,std::string_view key,const EntityCreation&);
    std::optional<EntityDescription> find(EntityId);
};
}

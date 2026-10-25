#include "fsnet/entity_catalog.hpp"
#include <openssl/sha.h>
#include <algorithm>

namespace fsnet {
namespace {
constexpr std::string_view sequence_key="fsnet/identity/high-water/v1";
std::string digest(std::string_view input) {
    std::string result(SHA256_DIGEST_LENGTH,'\0');
    SHA256(reinterpret_cast<const unsigned char*>(input.data()),input.size(),reinterpret_cast<unsigned char*>(result.data()));
    return result;
}
std::string operation_key(std::string_view scope,std::string_view key) {
    if(scope.empty() || scope.size()>256 || key.empty() || key.size()>128) throw std::invalid_argument("invalid creation key");
    std::string bytes; snapshot_detail::blob(bytes,scope); snapshot_detail::blob(bytes,key);
    return "fsnet/creation/v1/"+digest(bytes);
}
std::string creation_digest(const EntityCreation& creation) {
    if(creation.type.empty() || creation.type.size()>127 || creation.type.find_first_of(".\0",0,2)!=std::string::npos ||
       (!creation.owner_connection.empty() && !valid_connection_key(creation.owner_connection)) || creation.state.size()>8*1024*1024 ||
       (!creation.target_node.empty() && !valid_entity_node(creation.target_node)) ||
       (creation.persistence!=EntityPersistence::durable && creation.persistence!=EntityPersistence::rebuildable))
        throw std::invalid_argument("invalid entity creation");
    std::string bytes="FEC1";
    snapshot_detail::blob(bytes,creation.type); snapshot_detail::blob(bytes,creation.owner_connection);
    snapshot_detail::blob(bytes,creation.target_node); bytes.push_back(static_cast<char>(creation.persistence));
    snapshot_detail::blob(bytes,creation.state); return digest(bytes);
}
}
EntityIdAllocator::EntityIdAllocator(Storage& storage,std::uint64_t block_size) : storage_(storage),block_size_(block_size) {
    if(!block_size || block_size>1048576) throw std::invalid_argument("invalid entity identity block");
}
EntityId EntityIdAllocator::allocate() {
    std::lock_guard lock(mutex_);
    if(!remaining_) {
        // Contention is bounded: a storage job must not spin forever.
        for(unsigned attempt=0;attempt<128;++attempt) {
            auto previous=storage_.get(sequence_key); std::uint64_t high=0;
            if(previous) {
                std::string_view input=previous->value;
                if(input.size()!=12 || !input.starts_with("FEI1")) throw std::runtime_error("corrupt identity allocator");
                input.remove_prefix(4); high=entity_record_detail::integer(input);
            }
            if(high==UINT64_MAX) throw std::overflow_error("entity identities exhausted");
            auto count=std::min(block_size_,UINT64_MAX-high);
            std::string value="FEI1"; entity_record_detail::integer(value,high+count);
            if(!storage_.compare_exchange(sequence_key,previous?previous->version:0,value)) continue;
            next_=high+1; remaining_=count; break;
        }
        if(!remaining_) throw std::runtime_error("entity identity allocation contention");
    }
    auto id=EntityId(next_); --remaining_;
    if(remaining_) ++next_;
    return id;
}
EntityId EntityCatalog::create(std::string_view scope,std::string_view key,const EntityCreation& creation) {
    auto operation=operation_key(scope,key); auto fingerprint=creation_digest(creation);
    auto claim=storage_.get(operation);
    if(!claim) {
        auto candidate=allocator_.allocate();
        std::string bytes="FEC1"; entity_record_detail::integer(bytes,candidate.value()); bytes+=fingerprint;
        if(storage_.compare_exchange(operation,0,bytes)) claim=Record{std::move(bytes),0};
        else claim=storage_.get(operation);
    }
    if(!claim || claim->value.size()!=44 || !claim->value.starts_with("FEC1")) throw std::runtime_error("corrupt entity creation claim");
    std::string_view input=claim->value; input.remove_prefix(4); auto id=EntityId(entity_record_detail::integer(input));
    if(input!=fingerprint) throw EntityCreationConflict();
    auto entity_key=entity_record_key(id);
    EntityRecord initial; initial.initialized=false; initial.placement=creation.target_node;
    initial.snapshot.state=creation.state;
    initial.metadata=EntityMetadata{creation.type,creation.owner_connection,fingerprint,
        creation.persistence==EntityPersistence::rebuildable,false,
        creation.persistence==EntityPersistence::rebuildable?creation.state:std::string{}};
    auto stored=storage_.get(entity_key);
    if(!stored && storage_.compare_exchange(entity_key,0,encode_entity_record(initial))) return id;
    if(!stored) stored=storage_.get(entity_key);
    if(!stored) throw std::runtime_error("entity creation record disappeared");
    const auto record=decode_entity_record_view(stored->value);
    if(!record.metadata || record.metadata->type!=creation.type || record.metadata->creation_digest!=fingerprint)
        throw std::runtime_error("entity identity collision");
    return id; // A retry also returns a destroyed ID; it never resurrects it.
}
std::optional<EntityDescription> EntityCatalog::find(EntityId id) {
    auto stored=storage_.get(entity_record_key(id)); if(!stored) return {};
    auto record=decode_entity_record(stored->value);
    if(!record.metadata) throw std::runtime_error("global entity lacks metadata");
    return EntityDescription{id,std::move(record),stored->version};
}
}

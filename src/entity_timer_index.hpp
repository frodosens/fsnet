#pragma once
#include "fsnet/storage.hpp"
#include <map>

namespace fsnet::timer_index {
inline constexpr unsigned shards=64;
inline constexpr std::size_t per_shard=4096;
struct Entry {
    std::string type,id,token;
    std::uint64_t base_version=0,fence=0;
    std::int64_t due_ms=0;
    bool pending=true;
    std::string placement{}; // routing hint; pending entries must consult the entity
    bool operator==(const Entry&)const=default;
};
struct Bucket {std::uint64_t version=0;std::map<std::string,Entry> entries;};
std::string identity(std::string_view type,std::string_view id);
unsigned shard(std::string_view id);
Bucket read(Storage&,unsigned shard);
bool write(Storage&,unsigned shard,const Bucket&);
// Write-ahead discovery entry, before the owning entity CAS. The pending token
// is reconciled against the entity record; it must never be pruned merely for
// being older than wall time. Single-record Storage is sufficient.
void arm(Storage&,const Entry&);
}

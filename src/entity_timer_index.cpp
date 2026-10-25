#include "entity_timer_index.hpp"
#include "fsnet/entity_record.hpp"

namespace fsnet::timer_index {
namespace {
std::string storage_key(unsigned index){if(index>=shards)throw std::invalid_argument("timer index shard");return "fsnet/timers/index/v1/"+std::to_string(index);}
void valid(const Entry& entry){
    if(entry.type.empty() || entry.type.size()>127 || entry.type.find_first_of(".\0",0,2)!=std::string::npos || !valid_entity_id(entry.id) ||
       entry.token.empty() || entry.token.size()>64 || !entry.base_version || !entry.fence || entry.due_ms<0 || (!entry.placement.empty() && !valid_entity_node(entry.placement)))throw std::invalid_argument("invalid timer discovery entry");
}
}
std::string identity(std::string_view type,std::string_view id){std::string key(type);key+='\0';key+=id;return key;}
unsigned shard(std::string_view id){std::uint64_t hash=14695981039346656037ULL;for(unsigned char byte:id)hash=(hash^byte)*1099511628211ULL;return static_cast<unsigned>(hash%shards);}
Bucket read(Storage& db,unsigned index){
    auto record=db.get(storage_key(index));if(!record)return {};
    if(record->value.size()>1024*1024)throw std::runtime_error("timer discovery index limit");
    std::string_view input(record->value);bool placed=input.starts_with("FTI2");if(!placed && !input.starts_with("FTI1"))throw std::runtime_error("timer discovery version");input.remove_prefix(4);
    Bucket bucket;bucket.version=record->version;auto count=snapshot_detail::number(input);if(count>per_shard)throw std::runtime_error("timer discovery count");
    while(count--) {
        Entry entry;entry.type=snapshot_detail::blob(input);entry.id=snapshot_detail::blob(input);entry.token=snapshot_detail::blob(input);
        entry.base_version=entity_record_detail::integer(input);entry.fence=entity_record_detail::integer(input);auto due=entity_record_detail::integer(input);
        if(placed)entry.placement=snapshot_detail::blob(input);
        if(due>INT64_MAX || input.empty() || static_cast<unsigned char>(input.front())>1)throw std::runtime_error("timer discovery metadata");
        entry.due_ms=static_cast<std::int64_t>(due);entry.pending=input.front()!=0;input.remove_prefix(1);valid(entry);
        if(!bucket.entries.emplace(identity(entry.type,entry.id),entry).second)throw std::runtime_error("duplicate timer discovery entity");
    }
    if(!input.empty())throw std::runtime_error("trailing timer discovery bytes");
    return bucket;
}
bool write(Storage& db,unsigned index,const Bucket& bucket){
    if(bucket.entries.size()>per_shard)throw std::length_error("timer discovery capacity");
    std::string out="FTI2";snapshot_detail::number(out,static_cast<std::uint32_t>(bucket.entries.size()));
    for(const auto& [key,entry]:bucket.entries){
        valid(entry);if(key!=identity(entry.type,entry.id) || shard(entry.id)!=index)throw std::invalid_argument("timer discovery key");
        snapshot_detail::blob(out,entry.type);snapshot_detail::blob(out,entry.id);snapshot_detail::blob(out,entry.token);
        entity_record_detail::integer(out,entry.base_version);entity_record_detail::integer(out,entry.fence);entity_record_detail::integer(out,static_cast<std::uint64_t>(entry.due_ms));snapshot_detail::blob(out,entry.placement);out+=entry.pending?1:0;
    }
    if(out.size()>1024*1024)throw std::length_error("timer discovery byte capacity");
    return db.compare_exchange(storage_key(index),bucket.version,out);
}
void arm(Storage& db,const Entry& entry){
    valid(entry);auto index=shard(entry.id);auto key=identity(entry.type,entry.id);
    for(unsigned attempt=0;attempt<8;++attempt) {
        auto record=db.get(entity_record_key(EntityId(entry.id)));
        if(!record || record->version!=entry.base_version)throw std::runtime_error("timer producer lost its entity version");
        auto current=decode_entity_record_view(record->value);if(current.fence!=entry.fence)throw std::runtime_error("timer producer lost its fence");
        auto bucket=read(db,index);auto found=bucket.entries.find(key);
        if(found!=bucket.entries.end() && found->second.fence>entry.fence)throw std::runtime_error("stale timer discovery producer");
        bucket.entries.insert_or_assign(key,entry);if(write(db,index,bucket))return;
    }
    throw std::runtime_error("timer discovery contention");
}
}

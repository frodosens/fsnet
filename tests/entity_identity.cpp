#include "fsnet/entity_catalog.hpp"
#include "fsnet/database.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#include <unordered_set>
#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>
#endif

#define CHECK(c) do { if(!(c)) throw std::runtime_error(#c); } while(false)
using namespace fsnet;
template<class F> void throws(F function) { bool caught=false; try {function();} catch(const std::exception&) {caught=true;} CHECK(caught); }
struct FaultStorage final : Storage {
    std::shared_ptr<Storage> backing=memory_database();
    std::string fail_after;
    bool collide=false;
    std::optional<Record> get(std::string_view key) override {return backing->get(key);}
    bool compare_exchange(std::string_view key,std::uint64_t version,std::string_view value) override {
        if(collide && key.starts_with("fsnet/entity/v2/")) {
            collide=false; auto record=decode_entity_record(value); record.metadata->type="Impostor";
            CHECK(backing->compare_exchange(key,0,encode_entity_record(record))); return false;
        }
        auto result=backing->compare_exchange(key,version,value);
        if(result && !fail_after.empty() && key.starts_with(fail_after)) {fail_after.clear(); throw std::runtime_error("lost storage acknowledgement");}
        return result;
    }
    bool erase(std::string_view key,std::uint64_t version) override {return backing->erase(key,version);}
};
int main() {
    try {
        for(auto value:{1ULL,9007199254740993ULL,18446744073709551615ULL}) CHECK(EntityId(EntityId(value).str()).value()==value);
        for(auto bad:{"","0","0000000000000000","FFFFFFFFFFFFFFFF","000000000000000g"," 000000000000001"}) throws([&] {EntityId ignored(bad);});
        auto db=memory_database(); std::mutex mutex; std::unordered_set<std::uint64_t> ids;
        std::exception_ptr error; std::vector<std::thread> workers;
        for(unsigned worker=0;worker<8;++worker) workers.emplace_back([&] {
            try {
                EntityIdAllocator allocator(*db,17);
                for(unsigned i=0;i<300;++i) {auto id=allocator.allocate();std::lock_guard lock(mutex);CHECK(ids.insert(id.value()).second);}
            } catch(...) {std::lock_guard lock(mutex);error=std::current_exception();}
        });
        for(auto& worker:workers) worker.join(); if(error) std::rethrow_exception(error);
        CHECK(ids.size()==2400); EntityIdAllocator restarted(*db,17); CHECK(!ids.contains(restarted.allocate().value()));
        EntityCreation player{"Player","account-1","initial","map-b"};
        EntityCatalog catalog(*db,5); auto player_id=catalog.create("bootstrap","player-1",player);
        auto same=catalog.create("bootstrap","player-1",player); CHECK(same==player_id);
        auto described=catalog.find(player_id); CHECK(described && described->record.metadata->type=="Player");
        CHECK(!described->record.initialized && described->record.placement=="map-b" && described->record.transfer_id==0);
        CHECK(described->record.metadata->owner_account=="account-1");
        auto other=player;other.type="Pet"; CHECK(catalog.create("bootstrap","pet-1",other)!=player_id);
        throws([&] {catalog.create("bootstrap","player-1",other);});
        workers.clear();std::vector<std::uint64_t> creations(8);
        for(unsigned i=0;i<creations.size();++i) workers.emplace_back([&,i] {
            try {EntityCatalog contender(*db,3);creations[i]=contender.create("manager","same-operation",player).value();}
            catch(...) {std::lock_guard lock(mutex);error=std::current_exception();}
        });
        for(auto& worker:workers) worker.join();if(error)std::rethrow_exception(error);
        for(auto created:creations)CHECK(created==creations.front());
        for(auto fail:{"fsnet/identity/","fsnet/creation/","fsnet/entity/v2/"}) {
            FaultStorage faulty; faulty.fail_after=fail;
            {EntityCatalog first(faulty,4);throws([&] {first.create("manager","request-1",player);});}
            EntityCatalog retry(faulty,4); auto id=retry.create("manager","request-1",player);
            CHECK(retry.find(id)->record.metadata->type=="Player"); CHECK(retry.create("manager","request-1",player)==id);
        }
        FaultStorage collision;collision.collide=true;EntityCatalog guarded(collision);
        throws([&] {guarded.create("manager","collision",player);});
        // A terminal identity and its creation claim survive forever.
        auto terminal=described->record;terminal.metadata->destroyed=true;terminal.snapshot={};
        CHECK(db->compare_exchange(entity_record_key(player_id),described->version,encode_entity_record(terminal)));
        CHECK(catalog.create("bootstrap","player-1",player)==player_id && catalog.find(player_id)->record.metadata->destroyed);
        auto service=player;service.type="LoginEntity";service.persistence=EntityPersistence::rebuildable;
        auto service_id=catalog.create("manager","login-1",service); auto encoded=encode_entity_record(catalog.find(service_id)->record);
        auto view=decode_entity_record_view(encoded);CHECK(view.metadata->rebuildable && view.metadata->initial_state=="initial");
        for(std::size_t n=0;n<encoded.size();++n) throws([&] {decode_entity_record_view(std::string_view(encoded).substr(0,n));});
        auto max_db=memory_database();std::string high="FEI1";entity_record_detail::integer(high,UINT64_MAX-1);
        CHECK(max_db->compare_exchange("fsnet/identity/high-water/v1",0,high)); EntityIdAllocator last(*max_db,32);
        CHECK(last.allocate().value()==UINT64_MAX);throws([&] {last.allocate();});
#ifndef _WIN32
        // Independent processes use independent handles/allocators against the
        // same durable Storage. Restart reserves a new range after unused IDs.
        auto directory=std::filesystem::temp_directory_path()/("fsnet-identities-"+std::to_string(getpid()));
        std::filesystem::remove_all(directory);std::filesystem::create_directories(directory);
        struct Cleanup {std::filesystem::path path;~Cleanup(){std::filesystem::remove_all(path);}} cleanup{directory};
        std::vector<pid_t> children;
        for(unsigned index=0;index<3;++index) {
            auto pid=fork(); CHECK(pid>=0);
            if(pid==0) {
                try {
                    auto file=file_storage((directory/"store").string());EntityIdAllocator allocator(*file,31);
                    std::ofstream out(directory/std::to_string(index));
                    for(unsigned n=0;n<150;++n) out<<allocator.allocate().str()<<'\n';
                    out.close();_exit(0);
                } catch(...) {_exit(1);}
            }
            children.push_back(pid);
        }
        for(auto child:children) {int status=0;CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);}
        std::unordered_set<std::uint64_t> durable_ids;
        for(unsigned index=0;index<3;++index) {std::ifstream in(directory/std::to_string(index));std::string text;while(in>>text) CHECK(durable_ids.insert(EntityId(text).value()).second);}
        CHECK(durable_ids.size()==450);auto reopened=file_storage((directory/"store").string());EntityIdAllocator after_restart(*reopened,31);
        CHECK(!durable_ids.contains(after_restart.allocate().value()));
#endif
        std::cout<<"PASS global identity: concurrent ranges, process restart, idempotent creation, lost ACKs, collision rejection and terminal identity\n";
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}

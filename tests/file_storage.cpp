#include "fsnet/storage.hpp"
#include "fsnet/entity_catalog.hpp"
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>

#define CHECK(c) do { if (!(c)) throw std::runtime_error(#c); } while (false)
int main() {
    auto root = std::filesystem::temp_directory_path() / ("fsnet-files-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        auto first = fsnet::file_storage(root.string()), second = fsnet::file_storage(root.string());
        for(unsigned i=0;i<4096;++i)CHECK(!first->get("absent-"+std::to_string(i)));
        CHECK(std::filesystem::is_empty(root)); // a read miss must not consume persistent inodes
        CHECK(!first->supports_transactions()); bool unsupported = false;
        try { first->transact({}); } catch (const std::logic_error&) { unsupported = true; } CHECK(unsupported);
        const std::string key("../entity\0/42", 13), bytes("binary\0value\xff", 13);
        CHECK(first->compare_exchange(key, 0, bytes)); auto record = second->get(key); CHECK(record && record->value == bytes);
        CHECK(!second->compare_exchange(key, 0, "wrong")); CHECK(second->erase(key, record->version));
        CHECK(!first->get(key)); CHECK(first->compare_exchange(key, 0, "recreated"));
        CHECK(first->get(key)->version > record->version && !first->compare_exchange(key, record->version, "ABA"));
        CHECK(first->compare_exchange("counter", 0, "0"));
        std::atomic<bool> failed = false; std::vector<std::thread> threads;
        for (int i = 0; i < 4; ++i) threads.emplace_back([&, i] {
            try {
                auto store = i % 2 ? first : second;
                for (int j = 0; j < 25; ++j) for (;;) {
                    auto old = store->get("counter");
                    if (store->compare_exchange("counter", old->version, std::to_string(std::stoi(old->value) + 1))) break;
                }
            } catch (...) { failed = true; }
        });
        for (auto& thread : threads) thread.join(); CHECK(!failed && first->get("counter")->value == "100");
        fsnet::EntityCatalog catalog(*first);
        auto id=catalog.create("file","fenced",{"Counter","","saved-state","node-a"});
        auto entity_key=fsnet::entity_record_key(id);auto before=first->get(entity_key);
        auto entity=fsnet::decode_entity_record(before->value);entity.owner="node-b/boot";entity.fence=1;
        CHECK(second->compare_exchange(entity_key,before->version,fsnet::encode_entity_record(entity)));
        CHECK(!first->compare_exchange(entity_key,before->version,fsnet::encode_entity_record(entity)));
        first.reset(); second.reset(); auto reopened = fsnet::file_storage(root.string());
        CHECK(reopened->get("counter")->value == "100");
        std::filesystem::path victim;
        for (auto& path : std::filesystem::recursive_directory_iterator(root)) if (path.path().extension() == ".record") { victim = path.path(); break; }
        CHECK(!victim.empty());
        // Corrupt all committed records, ensuring reads fail closed rather than initializing empty state.
        for (auto& path : std::filesystem::recursive_directory_iterator(root)) if (path.path().extension() == ".record") {
            std::fstream file(path.path(), std::ios::in | std::ios::out | std::ios::binary); file.put('X');
        }
        bool corrupt = false; try { reopened->get("counter"); } catch (const std::runtime_error&) { corrupt = true; } CHECK(corrupt);
        reopened.reset(); std::filesystem::remove_all(root);
        std::cout << "PASS file storage: binary keys/values, cross-instance CAS, tombstones, fencing, reopening and corruption rejection without transactions\n";
    } catch (const std::exception& error) { std::filesystem::remove_all(root); std::cerr << error.what() << '\n'; return 1; }
}

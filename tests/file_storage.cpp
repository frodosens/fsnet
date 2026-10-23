#include "fsnet/storage.hpp"
#include "fsnet/entity.hpp"
#include "fsnet/entity_migration.hpp"
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
        fsnet::EntityStore owner_a(first), owner_b(second);
        CHECK(owner_a.transfer("entity", 0, "a")); auto fence = owner_a.owner("entity")->fence;
        CHECK(owner_a.save("entity", fence, 0, "state")); auto state = owner_a.load("entity");
        CHECK(owner_b.transfer("entity", fence, "b"));
        CHECK(!owner_a.save("entity", fence, state->version, "stale"));
        CHECK(owner_b.save("entity", owner_b.owner("entity")->fence, state->version, "restored"));
        CHECK(first->compare_exchange("fsnet/owner/old-only", 0, "old-owner"));
        bool legacy_rejected = false;
        try { owner_a.transfer("old-only", 0, "new"); } catch (const std::runtime_error&) { legacy_rejected = true; }
        CHECK(legacy_rejected && !first->get("fsnet/owned/old-only"));
        auto legacy = std::string("counter") + '\0' + "42";
        CHECK(first->compare_exchange("fsnet/actor/owner/" + legacy, 0, "100:old-owner"));
        CHECK(first->compare_exchange("fsnet/actor/state/" + legacy, 0, "saved-state"));
        bool active = false;
        try { fsnet::migrate_legacy_entity(*first, "counter", "42", 99); } catch (const std::runtime_error&) { active = true; }
        CHECK(active && !first->get(fsnet::entity_record_key("counter", "42")));
        CHECK(fsnet::migrate_legacy_entity(*first, "counter", "42", 101));
        auto migrated = fsnet::decode_entity_record(first->get(fsnet::entity_record_key("counter", "42"))->value);
        CHECK(migrated.owner.empty() && migrated.snapshot.state == "saved-state");
        CHECK(!fsnet::migrate_legacy_entity(*first, "counter", "42", 101));
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

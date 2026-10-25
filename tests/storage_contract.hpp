#pragma once
#include "fsnet/database.hpp"
#include <thread>
#include <vector>

inline void database_contract(std::shared_ptr<fsnet::Storage> db) {
    const std::string binary("value\0data", 10);
    CHECK(!db->get("player"));
    CHECK(db->compare_exchange("player", 0, binary));
    CHECK(!db->compare_exchange("player", 0, "duplicate"));
    CHECK(db->get("player")->value == binary);
    auto initial = db->get("player")->version;
    CHECK(db->compare_exchange("player", initial, "0"));
    CHECK(!db->compare_exchange("player", initial, "stale"));
    std::vector<std::thread> workers;
    for (int t = 0; t < 4; ++t) workers.emplace_back([db] {
        for (int n = 0; n < 50; ++n) {
            for (;;) {
                auto old = db->get("player");
                auto next = std::to_string(std::stoi(old->value) + 1);
                if (db->compare_exchange("player", old->version, next)) break;
            }
        }
    });
    for (auto& worker : workers) worker.join();
    auto record = db->get("player");
    CHECK(record->value == "200");
    CHECK(!db->erase("player", initial));
    CHECK(db->erase("player", record->version));
    CHECK(!db->get("player"));
    CHECK(!db->erase("player", record->version));
    CHECK(db->compare_exchange("player", 0, "recreated"));
    CHECK(!db->compare_exchange("player", initial, "ABA stale update"));
    CHECK(db->get("player")->version > record->version);
    if (!db->supports_transactions()) return;
    CHECK(db->transact({{"wallet:a", 0, "100"}, {"wallet:b", 0, "0"}}));
    auto a = db->get("wallet:a"), b = db->get("wallet:b");
    CHECK(!db->transact({{"wallet:a", a->version, "90"}, {"wallet:b", b->version + 1, "10"}}));
    CHECK(db->get("wallet:a")->value == "100");
    CHECK(db->transact({{"wallet:a", a->version, "90"}, {"wallet:b", b->version, "10"}}));
    CHECK(db->get("wallet:b")->value == "10");
}

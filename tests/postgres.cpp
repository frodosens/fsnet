#include "fsnet/database.hpp"
#include "fsnet/entity.hpp"
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <thread>

#define CHECK(c) do { if (!(c)) throw std::runtime_error(#c); } while (false)
#include "storage_contract.hpp"
int main(int argc, char** argv) {
    try {
        auto* uri = std::getenv("FSNET_TEST_POSTGRES_URI"); CHECK(uri);
        auto db = fsnet::postgres_database(uri);
        if (argc > 1 && std::string_view(argv[1]) == "--reconnect") {
            CHECK(db->compare_exchange("postgres/durable", 0, "acknowledged"));
            std::cout << "ready" << std::endl; std::string command; std::getline(std::cin, command);
            CHECK(command == "restarted"); CHECK(db->get("postgres/durable")->value == "acknowledged");
            std::cout << "PASS PostgreSQL: crash WAL recovery and safe read reconnect" << std::endl; return 0;
        }
        CHECK(!db->supports_transactions()); database_contract(db);
        const std::string binary_key("key\0bytes\xff", 10), binary_value("value\0bytes\xff", 12);
        CHECK(db->compare_exchange(binary_key, 0, binary_value)); CHECK(db->get(binary_key)->value == binary_value);
        CHECK(db->compare_exchange("cross-client", 0, "0"));
        std::vector<std::shared_ptr<fsnet::Database>> clients;
        for (int i = 0; i < 4; ++i) clients.push_back(fsnet::postgres_database(uri));
        std::atomic<bool> failed = false; std::vector<std::thread> workers;
        for (auto client : clients) workers.emplace_back([client, &failed] {
            try {
                for (int i = 0; i < 30; ++i) for (;;) {
                    auto old = client->get("cross-client");
                    if (client->compare_exchange("cross-client", old->version, std::to_string(std::stoi(old->value) + 1))) break;
                }
            } catch (...) { failed = true; }
        });
        for (auto& worker : workers) worker.join(); CHECK(!failed && db->get("cross-client")->value == "120");
        fsnet::EntityStore a(db), b(clients[0]);
        CHECK(a.transfer("postgres-entity", 0, "a")); auto fence = a.owner("postgres-entity")->fence;
        CHECK(a.save("postgres-entity", fence, 0, "state")); auto state = a.load("postgres-entity");
        CHECK(b.transfer("postgres-entity", fence, "b")); CHECK(!a.save("postgres-entity", fence, state->version, "stale"));
        CHECK(b.save("postgres-entity", b.owner("postgres-entity")->fence, state->version, "restored"));
        bool insecure = false;
        try { fsnet::postgres_database("postgresql://user@192.0.2.1/db?sslmode=disable"); } catch (const std::invalid_argument&) { insecure = true; }
        CHECK(insecure);
        auto* wrong = std::getenv("FSNET_TEST_POSTGRES_UNTRUSTED_URI"); CHECK(wrong); bool rejected = false;
        try { fsnet::postgres_database(wrong); } catch (const std::runtime_error&) { rejected = true; } CHECK(rejected);
        std::cout << "PASS PostgreSQL: storage contract, binary data, concurrent independent clients, single-record fencing and TLS verification\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}

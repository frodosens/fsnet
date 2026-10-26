#include "fsnet/database.hpp"
#include "fsnet/async_database.hpp"
#include "fsnet/entity_catalog.hpp"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>

#define CHECK(c) do { if (!(c)) throw std::runtime_error(#c); } while (false)
#include "storage_contract.hpp"
int main(int argc, char** argv) {
    try {
        if (argc > 1 && std::string_view(argv[1]) == "--tls-preflight") {
            bool rejected = false;
            try { fsnet::postgres_database("sslmode=disable"); }
            catch (const std::invalid_argument&) { rejected = true; }
            CHECK(rejected);
            return 0;
        }
        if (argc > 1 && std::string_view(argv[1]) == "--dns-preflight") {
            const auto started = std::chrono::steady_clock::now();
            for (const auto* options : {
                "host=must-not-resolve.invalid sslmode=verify-full",
                "host=localhost hostaddr=must-not-resolve.invalid sslmode=verify-full",
                "host=localhost,localhost hostaddr=127.0.0.1 sslmode=verify-full",
                "host=localhost hostaddr=127.0.0.1,127.0.0.1 sslmode=verify-full",
                "service=unexpanded-fixture sslmode=verify-full",
                "service=unexpanded-fixture host=server.fsnet sslmode=verify-full",
                "service=unexpanded-fixture hostaddr=127.0.0.1 sslmode=verify-full",
                "host=localhost,localhost,localhost,localhost,localhost,localhost,localhost,localhost,localhost sslmode=verify-full"}) {
                bool rejected = false;
                try { fsnet::postgres_database(options); }
                catch (const std::invalid_argument&) { rejected = true; }
                CHECK(rejected);
            }
            CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(1));
            std::cout << "PASS PostgreSQL: DNS/service and destination-list rejection before connection\n";
            return 0;
        }
        auto* uri = std::getenv("FSNET_TEST_POSTGRES_URI"); CHECK(uri);
        if (argc > 1 && std::string_view(argv[1]) == "--connect-blackhole") {
            auto started = std::chrono::steady_clock::now();
            bool failed = false;
            try { fsnet::postgres_database(uri); }
            catch (const std::runtime_error&) { failed = true; }
            const auto elapsed = std::chrono::steady_clock::now() - started;
            CHECK(failed && elapsed >= std::chrono::seconds(4) && elapsed < std::chrono::seconds(8));
            std::cout << "PASS PostgreSQL: bounded post-TCP startup deadline\n";
            return 0;
        }
        auto db = fsnet::postgres_database(uri);
        if (argc > 1 && std::string_view(argv[1]) == "--hostname") {
            CHECK(db->persistent() && !db->get("network/hostname"));
            std::cout << "PASS PostgreSQL: numeric hostaddr with verified logical TLS hostname\n";
            return 0;
        }
        if (argc > 1 && (std::string_view(argv[1]) == "--read-blackhole" || std::string_view(argv[1]) == "--write-blackhole")) {
            const bool write = std::string_view(argv[1]) == "--write-blackhole";
            std::cout << "ready" << std::endl;
            std::string command;
            std::getline(std::cin, command); CHECK(command == "blocked");
            fsnet::AsyncDatabase worker(db);
            fsnet::AsyncDatabase concurrent(db);
            bool completed = false, failed = false;
            bool concurrent_completed = false, concurrent_failed = false;
            const auto started = std::chrono::steady_clock::now();
            CHECK(worker.submit(1, [write](fsnet::Storage& storage) {
                if (write) (void)storage.compare_exchange("network/uncertain", 0, "committed-before-lost-reply");
                else (void)storage.get("network/read");
            }, [&](std::exception_ptr error) { completed = true; failed = bool(error); }));
            if (!write) CHECK(concurrent.submit(1, [](fsnet::Storage& storage) {
                (void)storage.get("network/concurrent-read");
            }, [&](std::exception_ptr error) { concurrent_completed = true; concurrent_failed = bool(error); }));
            worker.stop();
            concurrent.stop();
            CHECK(worker.poll() == 1 && completed && failed && worker.pending() == 0);
            if (!write) CHECK(concurrent.poll() == 1 && concurrent_completed && concurrent_failed && concurrent.pending() == 0);
            const auto elapsed = std::chrono::steady_clock::now() - started;
            CHECK(elapsed >= std::chrono::seconds(4) && elapsed < std::chrono::seconds(8));
            std::cout << "bounded" << std::endl;
            std::getline(std::cin, command); CHECK(command == "recovered");
            auto uncertain = db->get("network/uncertain");
            if (write) {
                // Losing the reply does not imply the write rolled back. It
                // must not be silently replayed or reported as definitely lost.
                CHECK(uncertain && uncertain->value == "committed-before-lost-reply");
                CHECK(!db->compare_exchange("network/uncertain", 0, "must-not-replay"));
                CHECK(db->erase("network/uncertain", uncertain->version));
            } else CHECK(!uncertain);
            CHECK(db->compare_exchange("network/healthy", 0, "recovered"));
            auto healthy = db->get("network/healthy"); CHECK(healthy && healthy->value == "recovered");
            CHECK(db->erase("network/healthy", healthy->version));
            std::cout << "PASS PostgreSQL: bounded blackhole operation/stop, uncertain write and reconnect\n";
            return 0;
        }
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
        std::vector<std::shared_ptr<fsnet::Storage>> clients;
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
        fsnet::EntityCatalog catalog(*db);auto id=catalog.create("postgres","fence",{"Counter","","state","node-a"});
        auto key=fsnet::entity_record_key(id);auto initial=db->get(key);auto record=fsnet::decode_entity_record(initial->value);
        record.owner="node-a/boot";record.fence=1;CHECK(db->compare_exchange(key,initial->version,fsnet::encode_entity_record(record)));
        auto old=db->get(key);record.owner="node-b/boot";record.fence=2;
        CHECK(clients[0]->compare_exchange(key,old->version,fsnet::encode_entity_record(record)));
        CHECK(!db->compare_exchange(key,old->version,fsnet::encode_entity_record(record)));
        bool insecure = false;
        try { fsnet::postgres_database("postgresql://user@192.0.2.1/db?sslmode=disable"); } catch (const std::invalid_argument&) { insecure = true; }
        CHECK(insecure);
        auto* wrong = std::getenv("FSNET_TEST_POSTGRES_UNTRUSTED_URI"); CHECK(wrong); bool rejected = false;
        try { fsnet::postgres_database(wrong); } catch (const std::runtime_error&) { rejected = true; } CHECK(rejected);
        std::cout << "PASS PostgreSQL: storage contract, binary data, concurrent independent clients, single-record fencing and TLS verification\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}

#include "fsnet/database.hpp"
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>

#define CHECK(c) do { if (!(c)) throw std::runtime_error(#c); } while (false)

int main() {
    const auto directory = std::filesystem::temp_directory_path() /
        ("fsnet-storage-capabilities-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        CHECK(!fsnet::memory_database()->persistent());
        CHECK(!fsnet::open_storage("")->persistent());
        CHECK(!fsnet::open_storage("memory:")->persistent());
        CHECK(!fsnet::open_storage("sqlite::memory:")->persistent());
        CHECK(!fsnet::open_storage("sqlite:")->persistent());
        std::filesystem::create_directories(directory);
        for (const auto& uri : {"file:" + (directory / "files").string(),
                                "sqlite:" + (directory / "records.sqlite").string()}) {
            {
                auto storage = fsnet::open_storage(uri);
                CHECK(storage->persistent());
                CHECK(storage->compare_exchange("persisted", 0, "value"));
            }
            auto restored = fsnet::open_storage(uri);
            CHECK(restored->persistent());
            CHECK(restored->get("persisted")->value == "value");
        }
        std::filesystem::remove_all(directory);
        std::cout << "PASS storage capabilities: volatile and temporary SQLite rejected, durable instances reopen\n";
    } catch (const std::exception& error) {
        std::filesystem::remove_all(directory);
        std::cerr << error.what() << '\n';
        return 1;
    }
}

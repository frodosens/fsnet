#include "fsnet/entity_migration.hpp"
#include <chrono>
#include <cstdlib>
#include <iostream>

int main(int argc, char** argv) {
    try {
        if (argc < 3 || argc > 4 || (argc == 4 && std::string_view(argv[3]) != "--raw-state"))
            throw std::invalid_argument("usage: fsnet-storage-migrate type id [--raw-state]; set FSNET_STORAGE_URI; stop all old writers first");
        auto* uri = std::getenv("FSNET_STORAGE_URI");
        if (!uri) throw std::runtime_error("set FSNET_STORAGE_URI");
        std::string type = argv[1], id = argv[2];
        if (type.empty() || type.size() > 127 || type.find('.') != std::string::npos || id.empty() || id.size() > 256)
            throw std::invalid_argument("invalid entity identity");
        auto storage = fsnet::open_storage(uri);
        auto now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        bool changed = fsnet::migrate_legacy_entity(*storage, type, id, now, argc == 4);
        std::cout << (changed ? "migrated" : "already current") << '\n';
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}

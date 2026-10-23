#include "fsnet/lua_engine.hpp"
#include "fsnet/entity_host.hpp"
#include <random>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <thread>

namespace { volatile std::sig_atomic_t running = 1; void stop(int) { running = 0; } }
int main(int argc, char** argv) {
    try {
        if (argc < 2 || argc > 3) throw std::invalid_argument("usage: fsnet-script script.lua [port]");
        const char* token = std::getenv("FSNET_CLUSTER_TOKEN");
        if (!token) throw std::runtime_error("set FSNET_CLUSTER_TOKEN");
        auto port = argc == 3 ? std::stoul(argv[2]) : 7001UL;
        if (port > 65535) throw std::invalid_argument("invalid port");
        std::ifstream file(argv[1], std::ios::binary | std::ios::ate);
        if (!file || file.tellg() < 0 || file.tellg() > 1024 * 1024) throw std::runtime_error("script missing or exceeds 1 MiB");
        std::string source(static_cast<std::size_t>(file.tellg()), '\0'); file.seekg(0);
        if (!file.read(source.data(), static_cast<std::streamsize>(source.size()))) throw std::runtime_error("cannot read script");
        fsnet::Node node(token);
        auto transport = std::make_shared<fsnet::NodeScriptHost>(node);
        const char* path = std::getenv("FSNET_DATABASE_PATH");
        auto database = path ? fsnet::sqlite_database(path) : fsnet::memory_database();
        std::string instance;
        std::random_device entropy;
        for (int i = 0; i < 4; ++i) instance += std::to_string(entropy()) + '-';
        fsnet::EntityRuntimeLimits entity_limits;
        if (const char* lease = std::getenv("FSNET_ENTITY_LEASE_MS")) {
            auto milliseconds = std::stoll(lease);
            if (milliseconds < 100 || milliseconds > 86400000) throw std::invalid_argument("invalid entity lease");
            entity_limits.lease = std::chrono::milliseconds(milliseconds);
            entity_limits.request_timeout = std::min(std::chrono::milliseconds(30000), entity_limits.lease / 2);
        }
        auto host = std::make_shared<fsnet::EntityScriptHost>(transport, database, instance, entity_limits);
        fsnet::LuaEngine engine(host); engine.load(source, argv[1]);
        std::signal(SIGINT, stop); std::signal(SIGTERM, stop);
        std::cout << "listening " << node.listen("127.0.0.1", static_cast<std::uint16_t>(port)) << std::endl;
        while (running) {
            node.poll(); host->poll(); engine.poll(); std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        engine.stop(); host->stop(); node.stop();
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}

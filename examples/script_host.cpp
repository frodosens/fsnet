#include "fsnet/lua_engine.hpp"
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
        auto host = std::make_shared<fsnet::NodeScriptHost>(node);
        fsnet::LuaEngine engine(host); engine.load(source, argv[1]);
        std::signal(SIGINT, stop); std::signal(SIGTERM, stop);
        std::cout << "listening " << node.listen("127.0.0.1", static_cast<std::uint16_t>(port)) << std::endl;
        while (running) {
            node.poll(); engine.poll(); std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        engine.stop(); node.stop();
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}

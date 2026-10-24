#include "fsnet/ice.hpp"
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <thread>

namespace { volatile std::sig_atomic_t running = 1; void stop(int) { running = 0; } }
int main(int argc, char** argv) {
    try {
        const char* user = std::getenv("FSNET_TURN_USER");
        const char* password = std::getenv("FSNET_TURN_PASSWORD");
        if (!user || !password) throw std::runtime_error("set FSNET_TURN_USER and FSNET_TURN_PASSWORD");
        fsnet::RelayConfig config;
        config.port = 3478;
        if (argc > 1) config.bind_address = argv[1];
        if (argc > 2) config.external_address = argv[2];
        config.relay_begin = 49160; config.relay_end = 49200;
        fsnet::TurnRelay relay(config);
        relay.credentials(user, password, 3600000, 8);
        std::signal(SIGINT, stop); std::signal(SIGTERM, stop);
        std::cout << "TURN listening " << relay.port() << std::endl;
        while (running) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}

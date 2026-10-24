#include "fsnet/node.hpp"
#include <algorithm>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <vector>

int main(int argc, char** argv) {
    try {
        using Clock = std::chrono::steady_clock;
        using namespace std::chrono_literals;
        auto total = argc > 1 ? std::stoul(argv[1]) : 10000UL;
        if (total == 0 || total > 10000000) throw std::invalid_argument("request count must be 1..10000000");
        fsnet::Node server("benchmark-only-token"), client("benchmark-only-token");
        server.bind("echo", [](std::string data, fsnet::Reply reply) { reply({fsnet::Status::ok, std::move(data)}); });
        auto peer = client.connect("127.0.0.1", server.listen("127.0.0.1", 0));
        std::size_t sent = 0, completed = 0;
        std::vector<double> latency;
        latency.reserve(total);
        std::function<void()> submit;
        submit = [&] {
            if (sent >= total) return;
            ++sent;
            auto begin = Clock::now();
            client.call(peer, "echo", std::string(128, 'x'), 5s, [&, begin](fsnet::Result result) {
                if (result.status != fsnet::Status::ok || result.data.size() != 128) throw std::runtime_error("benchmark RPC failed");
                latency.push_back(std::chrono::duration<double, std::milli>(Clock::now() - begin).count());
                ++completed; submit();
            });
        };
        auto begin = Clock::now();
        for (std::size_t i = 0; i < std::min<std::size_t>(total, 128); ++i) submit();
        while (completed < total) {
            server.poll(); client.poll();
            if (Clock::now() - begin > 60s) throw std::runtime_error("benchmark deadline exceeded");
        }
        double seconds = std::chrono::duration<double>(Clock::now() - begin).count();
        std::sort(latency.begin(), latency.end());
        std::cout << "loopback TCP, one thread, two nodes, payload=128 bytes, concurrency=128\n"
                  << "requests=" << total << " seconds=" << seconds << " requests_per_second=" << total / seconds
                  << " p50_ms=" << latency[total / 2] << " p99_ms=" << latency[(total - 1) * 99 / 100] << '\n';
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}

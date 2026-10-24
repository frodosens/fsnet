#include "../../examples/common/config.hpp"
#include "fsnet/node.hpp"
#include <cstdlib>
#include <iostream>

int main(int argc, char** argv) {
    try {
        const char* token = std::getenv("FSNET_CLUSTER_TOKEN");
        if (!token) throw std::runtime_error("set FSNET_CLUSTER_TOKEN");
        unsigned long port = argc > 1 ? std::stoul(argv[1]) : 7000;
        if (port > 65535) throw std::invalid_argument("invalid port");
        fsnet::Node node(token, {}, tls_from_environment());
        node.bind("system.echo", [](std::string request, fsnet::Reply reply) {
            reply({fsnet::Status::ok, std::move(request)});
        });
        std::cout << "listening " << node.listen(bind_from_environment(), static_cast<std::uint16_t>(port)) << std::endl;
        node.run();
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}

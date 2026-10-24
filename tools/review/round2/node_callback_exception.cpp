#include "fsnet/node.hpp"
#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>
using namespace std::chrono_literals;
int main() {
    std::set_terminate([] { std::cerr << "terminate_during_node_destructor=1\n"; std::_Exit(73); });
    auto node = std::make_unique<fsnet::Node>("local-review");
    auto peer = node->connect("127.0.0.1", 9);
    node->call(peer,"echo","",1s,[](fsnet::Result result) {
        std::cerr << "completion_status=" << static_cast<unsigned>(result.status) << '\n';
        throw std::runtime_error("application completion failed");
    });
    node.reset();
    std::cout << "destruction_completed=1\n";
}

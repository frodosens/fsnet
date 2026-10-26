#include "fsnet/node.hpp"
#include "support/transport.hpp"
#include <iostream>

using namespace std::chrono_literals;
namespace wire = testing::transport;
#define CHECK(condition) wire::require(bool(condition), #condition)

template<class Done> void until(fsnet::Node& node, Done done) {
    auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!done()) {
        node.poll(); CHECK(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(1ms);
    }
}

void authentication_tests() {
    const std::string secret = "review-secret-known-only-to-trusted-peer";
    fsnet::Node server(secret); unsigned called = 0;
    server.bind("protected", [&](std::string, fsnet::Reply reply) { ++called; reply({fsnet::Status::ok, "accepted"}); });
    auto port = server.listen("127.0.0.1", 0); asio::io_context io;
    auto connect = [&] {
        auto socket = std::make_unique<wire::Socket>(io);
        socket->connect({asio::ip::make_address("127.0.0.1"), port}); return socket;
    };
    auto pump = [&] { server.poll(); };
    auto reflected = connect(); auto challenge = wire::read_frame(*reflected, pump);
    CHECK(challenge.size() == 47 && challenge.find(secret) == std::string::npos);
    // Echoing the server's initial frame cannot establish knowledge of its key.
    wire::write(*reflected, wire::frame(0, {}, challenge.substr(14)));
    until(server, [&] { return server.stats().connections == 0; }); CHECK(called == 0);

    auto wrong_role = connect(); auto role_challenge = wire::read_frame(*wrong_role, pump);
    auto server_nonce = role_challenge.substr(15); std::string client_nonce(32, 'c');
    auto server_proof = std::string(1, '\2') + client_nonce + wire::proof(secret, true, server_nonce, client_nonce);
    wire::write(*wrong_role, wire::frame(0, {}, server_proof));
    until(server, [&] { return server.stats().connections == 0; }); CHECK(called == 0);

    auto trusted = connect(); std::string old_nonce;
    auto old_response = wire::authenticate(*trusted, pump, secret, &old_nonce);
    wire::write(*trusted, wire::frame(1, "protected", "", 1));
    auto answer = wire::read_frame(*trusted, pump); CHECK(answer.substr(14) == "accepted" && called == 1);
    trusted->close(); until(server, [&] { return server.stats().connections == 0; });
    auto replay = connect(); auto next_challenge = wire::read_frame(*replay, pump);
    CHECK(next_challenge.substr(15) != old_nonce);
    wire::write(*replay, wire::frame(0, {}, old_response));
    until(server, [&] { return server.stats().connections == 0; }); CHECK(called == 1);

    // A connector must verify the server's role proof before flushing queued RPC.
    asio::ip::tcp::acceptor listener(io, {asio::ip::make_address("127.0.0.1"), 0});
    fsnet::Node client(secret); auto peer = client.connect("127.0.0.1", listener.local_endpoint().port());
    std::optional<fsnet::Result> result;
    client.call(peer, "protected", "private application payload", 1s, [&](auto reply) { result = std::move(reply); });
    wire::Socket fake_server(io); listener.accept(fake_server);
    std::string fake_nonce(32, 's');
    wire::write(fake_server, wire::frame(0, {}, std::string(1, '\1') + fake_nonce));
    auto client_response = wire::read_frame(fake_server, [&] { client.poll(); });
    CHECK(client_response.size() == 79 && client_response[14] == 2);
    for (unsigned i = 0; i < 4; ++i) client.poll();
    std::array<char, 1> buffer{}; asio::error_code error;
    fake_server.read_some(asio::buffer(buffer), error);
    CHECK(error == asio::error::would_block || error == asio::error::try_again);
    // Reflect the client MAC with the server marker: distinct role domains reject it.
    wire::write(fake_server, wire::frame(0, {}, std::string(1, '\3') + client_response.substr(47)));
    until(client, [&] { return bool(result); });
    CHECK(result->status == fsnet::Status::disconnected && !client.connected(peer));
    client.stop(); server.stop();
}

void poll_budget_test() {
    fsnet::Limits limits; limits.poll_events = 4; limits.poll_time = 100ms;
    fsnet::Node server("budget", limits); unsigned received = 0;
    server.on_notification("event", [&](auto, const auto&, auto) { ++received; });
    auto port = server.listen("127.0.0.1", 0); asio::io_context io; wire::Socket socket(io);
    socket.connect({asio::ip::make_address("127.0.0.1"), port});
    wire::authenticate(socket, [&] { server.poll(); }, "budget");
    std::string burst; for (unsigned i = 0; i < 512; ++i) burst += wire::frame(3, "event", "");
    wire::write(socket, burst);
    unsigned scheduler_turns = 0;
    until(server, [&] {
        // Application scheduling gets a turn between Node polls while the socket
        // remains ready. A former unbounded poll drains all 512 in one call.
        ++scheduler_turns;
        CHECK(received <= scheduler_turns * limits.poll_events);
        return received == 512;
    });
    CHECK(scheduler_turns > 100); server.stop(); CHECK(server.stats().receive_bytes == 0);
}

void time_budget_test() {
    fsnet::Limits limits; limits.poll_events = 1024; limits.poll_time = 1ms;
    fsnet::Node server("time-budget", limits); unsigned received = 0;
    server.on_notification("slow", [&](auto, const auto&, auto) { ++received; std::this_thread::sleep_for(2ms); });
    auto port = server.listen("127.0.0.1", 0); asio::io_context io; wire::Socket socket(io);
    socket.connect({asio::ip::make_address("127.0.0.1"), port});
    wire::authenticate(socket, [&] { server.poll(); }, "time-budget");
    std::string burst; for (unsigned i = 0; i < 8; ++i) burst += wire::frame(3, "slow", "");
    wire::write(socket, burst);
    unsigned previous = 0;
    until(server, [&] {
        CHECK(received - previous <= 1); previous = received; return received == 8;
    });
    server.stop();
}

int main() {
    try {
        authentication_tests(); poll_budget_test(); time_budget_test();
        fsnet::Node server(""), client("");
        auto peer = client.connect("127.0.0.1", server.listen("127.0.0.1", 0)); bool done = false;
        client.call(peer, "system.health", "", 1s, [&](auto reply) { CHECK(reply.status == fsnet::Status::ok); done = true; });
        until(server, [&] { client.poll(); return done; });
        std::cout << "PASS bounded poll scheduling and mutual nonce/role authentication, reflection/replay rejection, proof-before-payload, empty loopback key\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}

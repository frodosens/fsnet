#include "fsnet/database.hpp"
#include "fsnet/cluster.hpp"
#include "fsnet/c_api.h"
#include "fsnet/entity.hpp"
#include "fsnet/directory.hpp"
#include "fsnet/ice.hpp"
#include "fsnet/node.hpp"
#include <asio.hpp>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
#define CHECK(condition) do { if (!(condition)) throw std::runtime_error(std::string(__FILE__) + ":" + std::to_string(__LINE__) + " " #condition); } while (false)
template<class Step, class Done> void until(Step step, Done done, std::chrono::milliseconds duration = 5s) {
    auto end = std::chrono::steady_clock::now() + duration;
    while (!done()) {
        step();
        if (std::chrono::steady_clock::now() >= end) throw std::runtime_error("test deadline exceeded");
        std::this_thread::sleep_for(1ms);
    }
}
#include "storage_contract.hpp"
void database_tests() {
    database_contract(fsnet::memory_database());
    auto file = (std::filesystem::temp_directory_path() / ("fsnet-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".db")).string();
    database_contract(fsnet::sqlite_database(file));
    { auto db = fsnet::sqlite_database(file); CHECK(db->compare_exchange("persist", 0, "saved")); }
    { auto db = fsnet::sqlite_database(file); CHECK(db->get("persist")->value == "saved"); }
    std::filesystem::remove(file);
    std::filesystem::remove(file + "-wal"); std::filesystem::remove(file + "-shm");
    std::cout << "PASS storage: interchangeable backends, CAS contention, deletion, restart persistence\n";
}
void fencing_test(std::shared_ptr<fsnet::Database> db) {
    fsnet::EntityStore first(db), second(db);
    CHECK(first.transfer("player42", 0, "logic-a"));
    auto old_owner = first.owner("player42"); CHECK(old_owner);
    CHECK(first.save("player42", old_owner->fence, 0, "saved before failure"));
    auto old_state = first.load("player42"); CHECK(old_state);
    CHECK(second.transfer("player42", old_owner->fence, "logic-b"));
    auto new_owner = second.owner("player42"); CHECK(new_owner);
    CHECK(new_owner->fence > old_owner->fence);
    CHECK(!first.save("player42", old_owner->fence, old_state->version, "stale writer after partition"));
    CHECK(second.save("player42", new_owner->fence, old_state->version, "recovered on logic-b"));
    CHECK(second.load("player42")->value == "recovered on logic-b");
    CHECK(!first.transfer("player42", old_owner->fence, "stale takeover"));
    std::cout << "PASS entity recovery: persisted state, atomic fencing rejects previous owner\n";
}
void node_tests() {
    fsnet::Limits limits;
    limits.frame_bytes = 4096; limits.queued_bytes = 8192; limits.pending_calls = 2;
    fsnet::Node server("test-token", limits), client("test-token", limits), stranger("wrong-token", limits);
    server.bind("echo", [](std::string data, fsnet::Reply reply) { reply({fsnet::Status::ok, data}); reply({fsnet::Status::internal, {}}); });
    server.bind("hang", [](std::string, fsnet::Reply) {});
    server.bind("throw", [](std::string, fsnet::Reply) { throw std::runtime_error("application error"); });
    auto port = server.listen("127.0.0.1", 0);
    auto peer = client.connect("127.0.0.1", port);
    auto pump = [&] { server.poll(); client.poll(); stranger.poll(); };
    bool done = false;
    client.call(peer, "echo", std::string("binary\0payload", 14), 1s, [&](fsnet::Result result) {
        CHECK(result.status == fsnet::Status::ok); CHECK(result.data == std::string("binary\0payload", 14)); done = true;
    });
    until(pump, [&] { return done; }); CHECK(client.pending() == 0);
    for (auto route : {"missing", "throw", "hang"}) {
        done = false;
        client.call(peer, route, {}, 30ms, [&](fsnet::Result result) {
            CHECK(result.status == (std::string(route) == "missing" ? fsnet::Status::not_found : std::string(route) == "throw" ? fsnet::Status::internal : fsnet::Status::timeout));
            done = true;
        });
        until(pump, [&] { return done; }); CHECK(client.pending() == 0);
    }
    int complete = 0;
    for (int i = 0; i < 3; ++i) client.call(peer, "hang", {}, 30ms, [&, i](fsnet::Result result) {
        CHECK(result.status == (i == 2 ? fsnet::Status::overloaded : fsnet::Status::timeout)); ++complete;
    });
    until(pump, [&] { return complete == 3; });
    done = false;
    auto bad_peer = stranger.connect("127.0.0.1", port);
    stranger.call(bad_peer, "echo", {}, 1s, [&](fsnet::Result result) { CHECK(result.status == fsnet::Status::disconnected); done = true; });
    until(pump, [&] { return done; });
    done = false;
    client.call(peer, "hang", {}, 1s, [&](fsnet::Result result) { CHECK(result.status == fsnet::Status::disconnected); done = true; });
    server.stop(); until(pump, [&] { return done; }); CHECK(client.pending() == 0);
    // Malformed peer must not allocate arbitrary memory or kill the server.
    fsnet::Node guarded("test-token", limits);
    auto guarded_port = guarded.listen("127.0.0.1", 0);
    asio::io_context io; asio::ip::tcp::socket socket(io);
    socket.connect({asio::ip::make_address("127.0.0.1"), guarded_port});
    std::array<unsigned char, 4> huge{255, 255, 255, 255};
    asio::write(socket, asio::buffer(huge));
    socket.non_blocking(true);
    bool hostile_closed = false;
    until([&] {
        guarded.poll();
        std::array<char, 128> buffer;
        asio::error_code error;
        socket.read_some(asio::buffer(buffer), error);
        if (error == asio::error::eof || error == asio::error::connection_reset) hostile_closed = true;
        else CHECK(!error || error == asio::error::would_block || error == asio::error::try_again);
    }, [&] { return hostile_closed; });
    socket.close(); guarded.stop();
    std::cout << "PASS RPC: binary payload, allowlist, exceptions, timeout, admission, bad auth, disconnect, hostile length\n";
}
void borrowed_call_lifetime() {
    fsnet::Node server("borrowed-input"),client("borrowed-input");
    server.bind("echo",[](std::string data,fsnet::Reply reply){reply({fsnet::Status::ok,std::move(data)});});
    auto peer=client.connect("127.0.0.1",server.listen("127.0.0.1",0));bool done=false;
    {
        std::string route="echo",payload(65536,'x');
        client.call(peer,std::string_view(route),std::string_view(payload),2s,[&](auto result){CHECK(result.status==fsnet::Status::ok && result.data==std::string(65536,'x'));done=true;});
        route.assign("invalid");payload.assign(65536,'y');
    }
    until([&]{server.poll();client.poll();},[&]{return done;});
}
void node_callback_lifetime() {
    // Destroy either endpoint in each callback context. ASAN catches service
    // lifetime errors after the callback returns into the active io_context.
    for (auto mode : {0, 1, 2, 3, 4, 5}) {
        auto server = std::make_unique<fsnet::Node>("callback-lifetime");
        auto client = std::make_unique<fsnet::Node>("callback-lifetime");
        server->bind("echo", [&](std::string data, fsnet::Reply reply) {
            if (mode == 1 || mode == 5) server.reset();
            else if (mode == 2) server->stop();
            else reply({fsnet::Status::ok, std::move(data)});
        });
        auto peer = client->connect("127.0.0.1", server->listen("127.0.0.1", 0));
        bool done = false;
        client->call(peer, "echo", "x", 1s, [&](auto result) {
            CHECK(result.status == (mode ? fsnet::Status::disconnected : fsnet::Status::ok));
            if (mode == 0 || mode == 3 || mode == 4) client.reset();
            done = true;
        });
        if (mode == 3) client->close(peer); // destruction from a synchronous close callback
        if (mode == 4) client->stop(); // destruction while stop is already on the stack
        if (mode == 5) {
            // Outbound frames are queued independently of receiving the token.
            for (int i = 0; i < 4; ++i) { client->poll(); std::this_thread::sleep_for(1ms); }
            server->run(); // handler destroys the Node driving this blocking call
        }
        until([&] { if (server) server->poll(); if (client) client->poll(); }, [&] { return done; });
    }
    std::cout << "PASS Node lifetime: callback destruction, close and stop reentry\n";
}
void node_receive_budget() {
    fsnet::Limits limits;
    limits.frame_bytes = 4096; limits.receive_bytes = 8192;
    fsnet::Node server("budget", limits);
    auto port = server.listen("127.0.0.1", 0);
    asio::io_context io;
    auto connect = [&] {
        auto socket = std::make_unique<asio::ip::tcp::socket>(io);
        socket->connect({asio::ip::make_address("127.0.0.1"), port});
        return socket;
    };
    std::array<unsigned char, 4> length{0, 0, 16, 0};
    auto hostile = connect();
    asio::write(*hostile, asio::buffer(length));
    for (int i = 0; i < 4; ++i) { server.poll(); std::this_thread::sleep_for(1ms); }
    until([&] { server.poll(); }, [&] { return server.stats().connections == 0; });
    // Authentication accepts only its exact frame length, before reserving body.
    CHECK(server.stats().receive_bytes == 0);
    hostile->close();
    auto authenticate = [&](auto& socket) {
        std::string frame(18, '\0'); frame[3] = 20; frame[4] = 1;
        frame += "budget";
        asio::write(*socket, asio::buffer(frame));
        for (int i = 0; i < 4; ++i) { server.poll(); std::this_thread::sleep_for(1ms); }
    };
    auto first = connect(); authenticate(first);
    asio::write(*first, asio::buffer(length));
    until([&] { server.poll(); }, [&] { return server.stats().receive_bytes == 4096; });
    auto second = connect(); authenticate(second);
    asio::write(*second, asio::buffer(length));
    until([&] { server.poll(); }, [&] { return server.stats().receive_bytes == 8192; });
    auto excess = connect(); authenticate(excess);
    for (int i = 0; i < 4; ++i) { server.poll(); std::this_thread::sleep_for(1ms); }
    CHECK(server.stats().receive_bytes == 8192);
    CHECK(server.stats().connections == 2);
    first->close();
    until([&] { server.poll(); }, [&] { return server.stats().receive_bytes == 4096; });
    auto replacement = connect(); authenticate(replacement);
    asio::write(*replacement, asio::buffer(length));
    until([&] { server.poll(); }, [&] { return server.stats().receive_bytes == 8192; });
    server.stop(); CHECK(server.stats().receive_bytes == 0);
    std::cout << "PASS receive admission: exact auth length, aggregate reservations and canceled-read release\n";
}
void c_api_lifetime_test() {
    static void* runtime = nullptr;
    runtime = fsnet_create("native-lifetime-token"); CHECK(runtime);
    CHECK(fsnet_bind(runtime, "close", [](std::uint64_t, const char*, std::size_t) {
        fsnet_destroy(runtime); runtime = nullptr;
    }) == 0);
    int port = fsnet_listen(runtime, "127.0.0.1", 0); CHECK(port > 0);
    fsnet::Node client("native-lifetime-token");
    bool finished = false;
    auto peer = client.connect("127.0.0.1", static_cast<std::uint16_t>(port));
    client.call(peer, "close", {}, 1s, [&](fsnet::Result result) {
        CHECK(result.status == fsnet::Status::disconnected); finished = true;
    });
    until([&] { if (runtime) CHECK(fsnet_poll(runtime) == 0); client.poll(); }, [&] { return finished; });
    CHECK(!runtime);
    std::cout << "PASS native bridge: destroy requested inside a callback is deferred safely\n";
}
void directory_tests() {
    fsnet::ServiceDirectory directory;
    auto now = std::chrono::steady_clock::now();
    directory.renew("player", 1, 1s, now); directory.renew("player", 2, 1s, now);
    auto old = directory.locate("player", "entity-42", now); CHECK(old);
    CHECK(directory.locate("player", "entity-42", now) == old);
    directory.remove(*old);
    CHECK(directory.locate("player", "entity-42", now) != old);
    CHECK(!directory.locate("player", "entity-42", now + 2s));
    std::cout << "PASS discovery: stable entity routing, peer removal, lease expiry\n";
}
void cluster_tests() {
    fsnet::Node first("cluster-token"), second("cluster-token"), gateway("cluster-token");
    auto bind = [](fsnet::Node& node, std::string name) {
        node.bind("player.get", [name](std::string, fsnet::Reply reply) { reply({fsnet::Status::ok, name}); });
    };
    bind(first, "first"); bind(second, "second");
    fsnet::ClusterClient cluster(gateway, {{"127.0.0.1", first.listen("127.0.0.1", 0), {"player"}},
                                          {"127.0.0.1", second.listen("127.0.0.1", 0), {"player"}}});
    auto pump = [&] { cluster.tick(); gateway.poll(); first.poll(); second.poll(); };
    until(pump, [&] { return cluster.available("player", "42"); });
    // Poll both initial health checks before selecting a replica.
    for (int i = 0; i < 20; ++i) { pump(); std::this_thread::sleep_for(1ms); }
    std::string selected;
    cluster.call("player", "42", "player.get", {}, 1s, [&](fsnet::Result result) { CHECK(result.status == fsnet::Status::ok); selected = result.data; });
    until(pump, [&] { return !selected.empty(); });
    if (selected == "first") first.stop(); else second.stop();
    std::string recovered;
    cluster.call("player", "42", "player.get", {}, 1s, [&](fsnet::Result result) { CHECK(result.status == fsnet::Status::ok); recovered = result.data; }, true);
    until(pump, [&] { return !recovered.empty(); });
    CHECK(recovered != selected);
    std::cout << "PASS cluster: health discovery and idempotent RPC failover after node stop\n";
}
void ice_test(bool relayed) {
    fsnet::TurnRelay relay;
    relay.credentials("test-user", "test-only-password-123456789", 60000, 4);
    fsnet::IceConfig config;
    config.bind_address = "127.0.0.1";
    if (relayed) {
        config.stun_host = "127.0.0.1"; config.stun_port = relay.port();
        config.turn_host = "127.0.0.1"; config.turn_port = relay.port();
        config.turn_user = "test-user"; config.turn_password = "test-only-password-123456789";
    }
    fsnet::IcePeer a(config), b(config);
    a.remote_description(b.description()); b.remote_description(a.description());
    a.gather(); b.gather();
    bool end_a = false, end_b = false;
    auto signal = [&] {
        auto batch_a = a.gathering(), batch_b = b.gathering();
        for (auto& candidate : batch_a.candidates) if (!relayed || candidate.find(" typ relay ") != std::string::npos) b.add_candidate(candidate);
        for (auto& candidate : batch_b.candidates) if (!relayed || candidate.find(" typ relay ") != std::string::npos) a.add_candidate(candidate);
        if (batch_a.done && !end_a) { b.remote_gathering_done(); end_a = true; }
        if (batch_b.done && !end_b) { a.remote_gathering_done(); end_b = true; }
    };
    until(signal, [&] { return a.connected() && b.connected(); }, 15s);
    if (relayed) {
        auto selected = a.selected_candidates() + "\n" + b.selected_candidates();
        std::cout << "Selected TURN candidates: " << selected << '\n';
        CHECK(selected.find(" typ relay ") != std::string::npos);
    }
    auto* sender = &a;
    auto* receiver = &b;
    // ICE can discover a peer-reflexive reverse route on loopback. Send on the
    // endpoint whose selected pair actually contains TURN, not that reverse path.
    if (relayed && a.selected_candidates().find(" typ relay ") == std::string::npos) {
        sender = &b; receiver = &a;
    }
    CHECK(!sender->send(std::string(1201, 'x')));
    std::vector<std::string> received;
    until([&] { signal(); sender->send("game-packet"); auto packets = receiver->receive(); received.insert(received.end(), packets.begin(), packets.end()); }, [&] { return !received.empty(); });
    CHECK(received.front() == "game-packet");
    if (relayed) CHECK(sender->selected_candidates().find(" typ relay ") != std::string::npos);
    std::cout << "PASS ICE " << (relayed ? "forced authenticated TURN relay" : "direct peer connectivity") << ": game datagram received\n";
}
int main() {
    try { database_tests(); fencing_test(fsnet::memory_database()); fencing_test(fsnet::sqlite_database(":memory:")); node_tests(); borrowed_call_lifetime(); node_callback_lifetime(); node_receive_budget(); c_api_lifetime_test(); directory_tests(); cluster_tests(); ice_test(false); ice_test(true); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}

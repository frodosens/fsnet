#include "fsnet/node.hpp"
#include "fsnet/c_api.h"
#include <chrono>
#include <iostream>
#include <optional>
#include <thread>

using namespace std::chrono_literals;
#define CHECK(c) do { if (!(c)) throw std::runtime_error(#c); } while (false)
int main(int argc, char** argv) {
    try {
        CHECK(argc == 2); std::string root = argv[1];
        auto config = [&](const char* name) { return fsnet::TlsConfig{root + "/ca.pem", root + '/' + name + ".pem", root + '/' + name + ".key", "server.fsnet"}; };
        fsnet::Node server("token", {}, config("server")), client("token", {}, config("client"));
        auto port = server.listen("127.0.0.1", 0); auto peer = client.connect("127.0.0.1", port);
        std::string authenticated; std::uint64_t incoming_peer=0;
        server.authorize([&](std::uint64_t incoming, const std::string& identity, const std::string& route) {
            CHECK(identity.starts_with("sha256:") && identity.size() == 71); authenticated = identity; incoming_peer=incoming;
            return route == "echo";
        });
        server.bind("echo", [](std::string data, fsnet::Reply reply) { reply({fsnet::Status::ok, std::move(data)}); });
        auto invoke = [&](fsnet::Node& node, std::uint64_t handle, const char* route) {
            std::optional<fsnet::Result> result;
            node.call(handle, route, std::string("binary\0data", 11), 1s, [&](fsnet::Result value) { result = std::move(value); });
            auto deadline = std::chrono::steady_clock::now() + 2s;
            while (!result) { server.poll(); node.poll(); CHECK(std::chrono::steady_clock::now() < deadline); std::this_thread::sleep_for(1ms); }
            return *result;
        };
        auto result = invoke(client, peer, "echo"); CHECK(result.status == fsnet::Status::ok && result.data == std::string("binary\0data", 11));
        CHECK(!authenticated.empty() && client.peer_identity(peer).size() == 71);
        CHECK(invoke(client, peer, "private").status == fsnet::Status::rejected);
        bool notified=false;
        client.on_notification("properties",[&](std::uint64_t from,const std::string& fingerprint,std::string data) {
            CHECK(from==peer && fingerprint.starts_with("sha256:") && data==std::string("binary\0data",11)); notified=true;
        });
        CHECK(server.notify(incoming_peer,"properties",std::string("binary\0data",11)));
        auto notification_deadline=std::chrono::steady_clock::now()+1s;
        while(!notified) { server.poll(); client.poll(); CHECK(std::chrono::steady_clock::now()<notification_deadline); std::this_thread::sleep_for(1ms); }
        CHECK(!server.notify(incoming_peer,"properties",std::string(1024*1024,'x')));
        client.off_notification("properties");

        server.authorize({});
        server.bind_rpc("client_only", {true, false, false}, [](const fsnet::CallIdentity& who, std::string, fsnet::Reply reply) {
            CHECK(who.caller == fsnet::Caller::client && who.subject.starts_with("sha256:"));
            reply({fsnet::Status::ok, "client"});
        });
        server.bind_rpc("server_only", {false, true, false}, [](const fsnet::CallIdentity& who, std::string, fsnet::Reply reply) {
            CHECK(who.caller == fsnet::Caller::server && who.subject == "trusted-service");
            reply({fsnet::Status::ok, "server"});
        });
        CHECK(invoke(client, peer, "client_only").status == fsnet::Status::ok);
        CHECK(invoke(client, peer, "server_only").status == fsnet::Status::rejected);
        const auto trusted_fingerprint = authenticated;
        server.identify([trusted_fingerprint](std::uint64_t, const std::string& fingerprint) -> std::optional<fsnet::CallIdentity> {
            if (fingerprint != trusted_fingerprint) return std::nullopt;
            return fsnet::CallIdentity{fsnet::Caller::server, "trusted-service"};
        });
        CHECK(invoke(client, peer, "server_only").status == fsnet::Status::ok);
        CHECK(invoke(client, peer, "client_only").status == fsnet::Status::rejected);
        server.identify([](std::uint64_t, const std::string&) -> std::optional<fsnet::CallIdentity> { throw std::runtime_error("identity failure"); });
        CHECK(invoke(client, peer, "server_only").status == fsnet::Status::rejected);
        server.identify({});
        fsnet::Node wrong_name("token", {}, config("client"));
        CHECK(invoke(wrong_name, wrong_name.connect("127.0.0.1", port, "wrong.fsnet"), "echo").status == fsnet::Status::disconnected);
        fsnet::Node rogue("token", {}, config("rogue"));
        CHECK(invoke(rogue, rogue.connect("127.0.0.1", port), "echo").status == fsnet::Status::disconnected);
        fsnet::Node certificate_only_server("", {}, config("server")), certificate_only_client("", {}, config("client"));
        auto certificate_peer = certificate_only_client.connect("127.0.0.1", certificate_only_server.listen("127.0.0.1", 0));
        std::optional<fsnet::Result> certificate_result;
        certificate_only_client.call(certificate_peer, "system.health", "", 1s, [&](fsnet::Result value) { certificate_result = std::move(value); });
        auto certificate_deadline = std::chrono::steady_clock::now() + 2s;
        while (!certificate_result) { certificate_only_server.poll(); certificate_only_client.poll(); CHECK(std::chrono::steady_clock::now() < certificate_deadline); std::this_thread::sleep_for(1ms); }
        CHECK(certificate_result->status == fsnet::Status::ok);
        fsnet::Node plaintext("token"); bool blocked = false;
        try { plaintext.listen("0.0.0.0", 0); } catch (const std::invalid_argument&) { blocked = true; } CHECK(blocked);
        blocked = false; try { plaintext.connect("192.0.2.1", 1234); } catch (const std::invalid_argument&) { blocked = true; } CHECK(blocked);
        CHECK(!fsnet_create_tls("token", (root + "/missing.pem").c_str(), (root + "/client.pem").c_str(), (root + "/client.key").c_str(), "server.fsnet"));
        auto* c_client = fsnet_create_tls("token", (root + "/ca.pem").c_str(), (root + "/client.pem").c_str(), (root + "/client.key").c_str(), "server.fsnet");
        CHECK(c_client); fsnet_destroy(c_client);
        std::cout << "PASS TLS 1.3: mutual certificate authentication, hostname verification, principal route authorization, untrusted certificate rejection and plaintext boundary\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}

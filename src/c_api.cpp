#include "fsnet/c_api.h"
#include "fsnet/node.hpp"
#include "fsnet/database.hpp"
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <unordered_map>

namespace {
thread_local std::string error;
using Clock = std::chrono::steady_clock;
struct Incoming { fsnet::Reply reply; Clock::time_point deadline; };
struct Runtime {
    fsnet::Node node;
    std::uint64_t serial = 0;
    std::unordered_map<std::uint64_t, Incoming> incoming;
    bool polling = false, close_requested = false, destroying = false;
    explicit Runtime(const char* token, fsnet::TlsConfig tls = {}) : node(token, {}, std::move(tls)) {}
    ~Runtime() { destroying = true; node.stop(); }
};
template<class F> int guard(F function) {
    error.clear();
    try { return function(); }
    catch (const std::exception& exception) { error = exception.what(); return -1; }
    catch (...) { error = "unknown native error"; return -1; }
}
Runtime& runtime(void* node) {
    if (!node) throw std::invalid_argument("null node");
    auto& self = *static_cast<Runtime*>(node);
    if (self.destroying) throw std::logic_error("node is closing");
    return self;
}
fsnet::Database& database(void* db) {
    if (!db) throw std::invalid_argument("null database");
    return **static_cast<std::shared_ptr<fsnet::Database>*>(db);
}
std::string bytes(const char* data, size_t length) {
    if (length > 16 * 1024 * 1024 || (!data && length)) throw std::invalid_argument("invalid bytes");
    return length ? std::string(data, length) : std::string{};
}
const char* text(const char* value) { if (!value) throw std::invalid_argument("null text"); return value; }
void port_check(int port) { if (port < 0 || port > 65535) throw std::invalid_argument("invalid port"); }
}
extern "C" {
const char* fsnet_last_error() { return error.c_str(); }
void* fsnet_create(const char* token) {
    Runtime* result = nullptr; guard([&] { result = new Runtime(text(token)); return 0; }); return result;
}
void* fsnet_create_tls(const char* token, const char* ca, const char* certificate, const char* key, const char* peer_name) {
    Runtime* result = nullptr;
    guard([&] { result = new Runtime(text(token), {text(ca), text(certificate), text(key), peer_name ? peer_name : ""}); return 0; });
    return result;
}
int fsnet_authorize(void* node, fsnet_authorizer callback) {
    return guard([&] {
        runtime(node).node.authorize(callback ? fsnet::Node::Authorizer([callback](std::uint64_t peer, const std::string& identity, const std::string& route) {
            return callback(peer, identity.c_str(), route.c_str()) == 1;
        }) : fsnet::Node::Authorizer{}); return 0;
    });
}
void fsnet_destroy(void* node) {
    guard([&] {
        auto* self = static_cast<Runtime*>(node);
        if (!self || self->destroying) return 0;
        if (self->polling) self->close_requested = true;
        else delete self;
        return 0;
    });
}
int fsnet_listen(void* node, const char* address, int port) {
    return guard([&] { port_check(port); return int(runtime(node).node.listen(text(address), static_cast<std::uint16_t>(port))); });
}
int64_t fsnet_connect(void* node, const char* address, int port) {
    std::int64_t peer = -1;
    guard([&] { port_check(port); peer = static_cast<std::int64_t>(runtime(node).node.connect(text(address), static_cast<std::uint16_t>(port))); return 0; });
    return peer;
}
int fsnet_bind(void* pointer, const char* route, fsnet_handler callback) {
    return guard([&] {
        if (!callback) throw std::invalid_argument("null handler");
        auto& self = runtime(pointer);
        self.node.bind(text(route), [&self, callback](std::string data, fsnet::Reply reply) {
            if (self.incoming.size() >= 4096) return reply({fsnet::Status::overloaded, {}});
            auto id = ++self.serial;
            self.incoming.emplace(id, Incoming{std::move(reply), Clock::now() + std::chrono::seconds(30)});
            callback(id, data.data(), data.size());
        }); return 0;
    });
}
int fsnet_respond(void* pointer, uint64_t id, int status, const char* data, size_t length) {
    return guard([&] {
        if (status < 0 || status > static_cast<int>(fsnet::Status::internal)) throw std::invalid_argument("invalid status");
        auto payload = bytes(data, length);
        auto& self = runtime(pointer);
        auto it = self.incoming.find(id);
        if (it == self.incoming.end()) return 0;
        auto reply = std::move(it->second.reply); self.incoming.erase(it);
        reply({static_cast<fsnet::Status>(status), std::move(payload)}); return 1;
    });
}
int fsnet_call(void* pointer, uint64_t peer, const char* route, const char* data, size_t length, int timeout, fsnet_completion callback) {
    return guard([&] {
        if (!callback) throw std::invalid_argument("null completion");
        runtime(pointer).node.call(peer, text(route), bytes(data, length), std::chrono::milliseconds(timeout),
          [callback](fsnet::Result result) { callback(static_cast<int>(result.status), result.data.data(), result.data.size()); });
        return 0;
    });
}
int fsnet_poll(void* pointer) {
    return guard([&] {
        auto& self = runtime(pointer);
        if (self.polling) throw std::logic_error("recursive poll");
        self.polling = true;
        try { self.node.poll(); }
        catch (...) { self.polling = false; throw; }
        std::vector<fsnet::Reply> expired;
        auto now = Clock::now();
        for (auto it = self.incoming.begin(); it != self.incoming.end();) {
            if (it->second.deadline <= now) { expired.push_back(std::move(it->second.reply)); it = self.incoming.erase(it); }
            else ++it;
        }
        for (auto& reply : expired) reply({fsnet::Status::timeout, {}});
        self.polling = false;
        if (self.close_requested) delete &self;
        return 0;
    });
}
void* fsnet_db_open(const char* filename) {
    void* result = nullptr;
    guard([&] {
        auto file = std::string(text(filename));
        result = new std::shared_ptr<fsnet::Database>(file.empty() ? fsnet::memory_database() : fsnet::sqlite_database(file));
        return 0;
    }); return result;
}
void* fsnet_store_open(const char* uri) {
    std::shared_ptr<fsnet::Storage>* result = nullptr;
    guard([&] { result = new std::shared_ptr<fsnet::Storage>(fsnet::open_storage(text(uri))); return 0; }); return result;
}
void fsnet_db_close(void* db) { delete static_cast<std::shared_ptr<fsnet::Database>*>(db); }
char* fsnet_db_get(void* db, const char* key, size_t key_length, size_t* length, uint64_t* version) {
    char* result = nullptr;
    guard([&] {
        if (!length || !version) throw std::invalid_argument("null output");
        *length = 0; *version = 0;
        auto record = database(db).get(bytes(key, key_length));
        if (!record) return 0;
        result = static_cast<char*>(std::malloc(record->value.size() + 1));
        if (!result) throw std::bad_alloc();
        std::memcpy(result, record->value.data(), record->value.size());
        *length = record->value.size(); *version = record->version; return 1;
    }); return result;
}
int fsnet_db_cas(void* db, const char* key, size_t key_length, uint64_t version, const char* value, size_t length) {
    return guard([&] { return database(db).compare_exchange(bytes(key, key_length), version, bytes(value, length)) ? 1 : 0; });
}
int fsnet_db_erase(void* db, const char* key, size_t length, uint64_t version) {
    return guard([&] { return database(db).erase(bytes(key, length), version) ? 1 : 0; });
}
void fsnet_free(void* pointer) { std::free(pointer); }
}

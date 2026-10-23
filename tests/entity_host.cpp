#include "fsnet/entity_host.hpp"
#include "fsnet/entity_message.hpp"
#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>
#include <unordered_map>

using namespace std::chrono_literals;
#define CHECK(c) do { if (!(c)) throw std::runtime_error(std::string(__FILE__) + ":" + std::to_string(__LINE__) + " " #c); } while (false)
struct Host final : fsnet::ScriptHost {
    std::unordered_map<std::string, fsnet::Handler> handlers;
    bool has_route(const std::string& r) const override { return handlers.contains(r); }
    void bind(std::string r, fsnet::Handler h) override { CHECK(handlers.emplace(r, std::move(h)).second); }
    void unbind(const std::string& r) override { handlers.erase(r); }
    std::uint64_t connect(const std::string&, std::uint16_t) override { return 1; }
    void call(std::uint64_t, std::string, std::string, std::chrono::milliseconds, fsnet::Reply) override { throw std::logic_error("unused"); }
};
template<class Predicate> void until(fsnet::EntityScriptHost& runtime, Predicate done) {
    auto deadline = std::chrono::steady_clock::now() + 3s;
    while (!done()) { runtime.poll(); CHECK(std::chrono::steady_clock::now() < deadline); std::this_thread::sleep_for(1ms); }
}
fsnet::Result invoke(fsnet::EntityScriptHost& runtime, Host& host, const std::string& method, const std::string& id, const std::string& data = "") {
    std::optional<fsnet::Result> result;
    host.handlers.at(method)(fsnet::entity_message(id, data), [&](fsnet::Result value) { result = std::move(value); });
    until(runtime, [&] { return result.has_value(); }); return *result;
}
void bind(fsnet::EntityScriptHost& runtime) {
    runtime.bind_stateful("counter.append", "counter", "initial", [](std::string input, fsnet::Reply done) {
        auto entity = fsnet::parse_entity_message(input); CHECK(entity);
        auto state = fsnet::parse_state_message(entity->payload); CHECK(state);
        auto next = state->state + state->payload;
        done({fsnet::Status::ok, fsnet::state_message(next, next)});
    });
    runtime.bind_stateful("counter.fail", "counter", "initial", [](std::string, fsnet::Reply done) { done({fsnet::Status::internal, {}}); });
}
void ownership() {
    auto db = fsnet::memory_database();
    auto parent_a = std::make_shared<Host>(), parent_b = std::make_shared<Host>();
    std::atomic<std::int64_t> now{100000}; auto clock = [&] { return now.load(); };
    fsnet::EntityScriptHost a(parent_a, db, "node-a/boot-1", {}, {}, clock), b(parent_b, db, "node-b/boot-1", {}, {}, clock);
    bind(a); bind(b);
    CHECK(invoke(a, *parent_a, "counter.append", "42", "a").data == "initiala");
    CHECK(invoke(b, *parent_b, "counter.append", "42", "wrong").status == fsnet::Status::rejected);
    CHECK(invoke(a, *parent_a, "counter.fail", "42").status == fsnet::Status::internal);
    fsnet::Reply suspended;
    a.bind_stateful("counter.wait", "counter", "initial", [&](std::string, fsnet::Reply done) { suspended = std::move(done); });
    std::optional<fsnet::Result> stale;
    parent_a->handlers.at("counter.wait")(fsnet::entity_message("42", ""), [&](fsnet::Result result) { stale = std::move(result); });
    until(a, [&] { return bool(suspended); });
    now += 60001;
    CHECK(invoke(b, *parent_b, "counter.append", "42", "b").data == "initialab");
    suspended({fsnet::Status::ok, fsnet::state_message("stale", "wrong")});
    until(a, [&] { return stale.has_value(); }); CHECK(stale->status == fsnet::Status::rejected);
    CHECK(invoke(b, *parent_b, "counter.append", "42").data == "initialab");
    auto old_handle = b.handle("counter", "42"); CHECK(old_handle);
    std::optional<fsnet::Result> released;
    b.deactivate(*old_handle, [&](fsnet::Result result) { released = std::move(result); });
    CHECK(invoke(b, *parent_b, "counter.append", "42").status == fsnet::Status::rejected); // admission while releasing
    until(b, [&] { return released.has_value(); }); CHECK(released->status == fsnet::Status::ok && !b.handle("counter", "42"));
    CHECK(invoke(a, *parent_a, "counter.append", "42", "c").data == "initialabc"); // released lease permits immediate handoff
    released.reset(); b.deactivate(*old_handle, [&](fsnet::Result result) { released = std::move(result); });
    CHECK(released && released->status == fsnet::Status::rejected);
    CHECK(a.stats().pending == 0 && b.stats().pending == 0);
    std::cout << "PASS persistent entities: restore, lease contention, takeover, stale commit rejection, handoff and stale handles\n";
}
void fencing() {
    auto db = fsnet::memory_database(); auto parent = std::make_shared<Host>();
    fsnet::EntityScriptHost runtime(parent, db, "owner");
    fsnet::Reply finish;
    runtime.bind_stateful("counter.wait", "counter", "initial", [&](std::string, fsnet::Reply done) { finish = std::move(done); });
    std::optional<fsnet::Result> result;
    parent->handlers.at("counter.wait")(fsnet::entity_message("42", ""), [&](fsnet::Result value) { result = std::move(value); });
    until(runtime, [&] { return bool(finish); });
    const auto key = (std::string("fsnet/actor/owner/counter") + '\0') + "42";
    auto record = db->get(key); CHECK(record);
    CHECK(db->compare_exchange(key, record->version, "9999999999999:other-owner")); // explicit administrative transfer before expiry
    finish({fsnet::Status::ok, fsnet::state_message("stale", "wrong")});
    until(runtime, [&] { return result.has_value(); }); CHECK(result->status == fsnet::Status::rejected);
    CHECK(!db->get((std::string("fsnet/actor/state/counter") + '\0') + "42"));
    std::cout << "PASS entity fencing: ownership version is checked atomically with state writes\n";
}
void receipts() {
    auto db = fsnet::memory_database(); auto parent = std::make_shared<Host>();
    fsnet::EntityRuntimeLimits limits; limits.receipts = 2;
    fsnet::EntityScriptHost runtime(parent, db, "owner", limits); bind(runtime);
    auto call = [&](const std::string& token, const std::string& data) {
        std::optional<fsnet::Result> result;
        parent->handlers.at("counter.append")(fsnet::entity_message("42", data, token), [&](fsnet::Result value) { result = std::move(value); });
        until(runtime, [&] { return result.has_value(); }); return *result;
    };
    CHECK(call("request-1", "a").data == "initiala");
    CHECK(call("request-1", "a").data == "initiala");
    CHECK(call("request-1", "changed").status == fsnet::Status::rejected);
    CHECK(invoke(runtime, *parent, "counter.append", "42").data == "initiala");
    CHECK(call("request-2", "b").data == "initialab");
    CHECK(call("request-3", "c").data == "initialabc");
    CHECK(call("request-3", "c").data == "initialabc");
    CHECK(call("request-1", "a").data == "initialabca"); // receipt retention is bounded, not infinite exactly-once
    CHECK(runtime.stats().replayed == 2 && runtime.stats().state_bytes == 0);
    auto handle = runtime.handle("counter", "42"); std::optional<fsnet::Result> release;
    runtime.deactivate(*handle, [&](fsnet::Result result) { release = std::move(result); });
    until(runtime, [&] { return release.has_value(); }); CHECK(release->status == fsnet::Status::ok);
    auto new_parent = std::make_shared<Host>(); fsnet::EntityScriptHost recovered(new_parent, db, "new-owner", limits); bind(recovered);
    std::optional<fsnet::Result> replay;
    new_parent->handlers.at("counter.append")(fsnet::entity_message("42", "a", "request-1"), [&](fsnet::Result value) { replay = std::move(value); });
    until(recovered, [&] { return replay.has_value(); }); CHECK(replay->data == "initialabca" && recovered.stats().replayed == 1);
    std::cout << "PASS entity receipts: duplicate replay, token conflict, bounded retention and recovery on a new owner\n";
}
void ticks_and_limits() {
    auto parent = std::make_shared<Host>(); fsnet::EntityRuntimeLimits limits;
    limits.entities = 1; limits.requests = 1; limits.tick_interval = 2ms;
    fsnet::EntityScriptHost runtime(parent, fsnet::memory_database(), "owner", limits);
    bind(runtime);
    runtime.bind_stateful("counter.$tick", "counter", "initial", [](std::string input, fsnet::Reply done) {
        auto message = fsnet::parse_entity_message(input); CHECK(message);
        auto state = fsnet::parse_state_message(message->payload); CHECK(state && state->payload == "2");
        done({fsnet::Status::ok, fsnet::state_message(state->state + ".", "")});
    });
    CHECK(invoke(runtime, *parent, "counter.append", "42").status == fsnet::Status::ok);
    CHECK(invoke(runtime, *parent, "counter.append", "43").status == fsnet::Status::overloaded);
    auto committed = runtime.stats().committed;
    until(runtime, [&] { return runtime.stats().committed > committed; });
    CHECK(runtime.stats().tick_failures == 0);
    runtime.stop(); CHECK(parent->handlers.empty() && runtime.stats().pending == 0);
    std::cout << "PASS entity ticks: bounded admission, serialized ticks and draining shutdown\n";
}
int main() {
    try { ownership(); fencing(); receipts(); ticks_and_limits(); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}

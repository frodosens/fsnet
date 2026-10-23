#include "fsnet/entity_host.hpp"
#include "fsnet/entity_message.hpp"
#include "fsnet/entity_record.hpp"
#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>
#include <unordered_map>

using namespace std::chrono_literals;
#define CHECK(c) do { if (!(c)) throw std::runtime_error(std::string(__FILE__) + ":" + std::to_string(__LINE__) + " " #c); } while (false)
struct Host final : fsnet::ScriptHost {
    std::unordered_map<std::string, fsnet::Handler> handlers;
    std::unordered_map<std::string, fsnet::ContextHandler> contexts;
    void bind_context(std::string r, fsnet::RpcPolicy, fsnet::ContextHandler h) override { CHECK(contexts.emplace(std::move(r),std::move(h)).second); }
    bool has_route(const std::string& r) const override { return handlers.contains(r) || contexts.contains(r); }
    void bind(std::string r, fsnet::Handler h) override { CHECK(handlers.emplace(r, std::move(h)).second); }
    void unbind(const std::string& r) override { handlers.erase(r); contexts.erase(r); }
    std::uint64_t connect(const std::string&, std::uint16_t) override { return 1; }
    void call(std::uint64_t, std::string, std::string, std::chrono::milliseconds, fsnet::Reply) override { throw std::logic_error("unused"); }
};
template<class Predicate> void until(fsnet::EntityScriptHost& runtime, Predicate done) {
    auto deadline = std::chrono::steady_clock::now() + 3s;
    while (!done()) { runtime.poll(); CHECK(std::chrono::steady_clock::now() < deadline); std::this_thread::sleep_for(1ms); }
}
class NoTransactions final : public fsnet::Storage {
    std::shared_ptr<fsnet::Storage> parent = fsnet::memory_database();
public:
    std::optional<fsnet::Record> get(std::string_view key) override { return parent->get(key); }
    bool compare_exchange(std::string_view key, std::uint64_t expected, std::string_view value) override { return parent->compare_exchange(key, expected, value); }
    bool erase(std::string_view key, std::uint64_t expected) override { return parent->erase(key, expected); }
    // No transact implementation: every accidental use throws.
};
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
    auto db = std::make_shared<NoTransactions>();
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
    auto db = std::make_shared<NoTransactions>(); auto parent = std::make_shared<Host>();
    fsnet::EntityScriptHost runtime(parent, db, "owner");
    fsnet::Reply finish;
    runtime.bind_stateful("counter.wait", "counter", "initial", [&](std::string, fsnet::Reply done) { finish = std::move(done); });
    std::optional<fsnet::Result> result;
    parent->handlers.at("counter.wait")(fsnet::entity_message("42", ""), [&](fsnet::Result value) { result = std::move(value); });
    until(runtime, [&] { return bool(finish); });
    const auto key = fsnet::entity_record_key("counter", "42");
    auto record = db->get(key); CHECK(record);
    auto moved = fsnet::decode_entity_record(record->value); moved.owner = "other-owner"; ++moved.fence;
    CHECK(db->compare_exchange(key, record->version, fsnet::encode_entity_record(moved)));
    finish({fsnet::Status::ok, fsnet::state_message("stale", "wrong")});
    until(runtime, [&] { return result.has_value(); }); CHECK(result->status == fsnet::Status::rejected);
    auto stored = fsnet::decode_entity_record(db->get(key)->value);
    CHECK(stored.owner == "other-owner" && stored.snapshot.state == "initial");
    std::cout << "PASS entity fencing: ownership and state are replaced with single-record CAS; transactions are unsupported\n";
}
void receipts() {
    auto db = std::make_shared<NoTransactions>(); auto parent = std::make_shared<Host>();
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
void owner_recheck() {
    auto storage=std::make_shared<NoTransactions>(); auto parent=std::make_shared<Host>();
    fsnet::EntityScriptHost runtime(parent,storage,"context/boot"); std::string owner="alice";
    runtime.ownership([&](const std::string&,const std::string&) { return owner; });
    fsnet::Reply suspended;
    runtime.bind_stateful_context("counter.wait","counter","initial",{true,false,true},
        [&](const fsnet::CallIdentity&,std::string,fsnet::Reply reply) { suspended=std::move(reply); });
    runtime.bind_stateful_context("counter.read","counter","initial",{true,false,true},
        [](const fsnet::CallIdentity&,std::string,fsnet::Reply reply) { reply({fsnet::Status::ok,fsnet::state_message("initial","private")}); });
    std::optional<fsnet::Result> result;
    auto done=[&](fsnet::Result value) { result=std::move(value); };
    parent->contexts.at("counter.wait")({fsnet::Caller::client,"alice",1},fsnet::entity_message("42",""),done);
    until(runtime,[&] { return bool(suspended); }); owner="bob";
    suspended({fsnet::Status::ok,fsnet::state_message("bad","private")});
    until(runtime,[&] { return result.has_value(); }); CHECK(result->status==fsnet::Status::rejected && result->data.empty());
    CHECK(fsnet::decode_entity_record(storage->get(fsnet::entity_record_key("counter","42"))->value).snapshot.state=="initial");
    owner="alice"; result.reset();
    auto request=fsnet::entity_message("42","","private-id");
    parent->contexts.at("counter.read")({fsnet::Caller::client,"alice",1},request,done);
    until(runtime,[&] { return result.has_value(); }); CHECK(result->data=="private");
    result.reset(); parent->contexts.at("counter.read")({fsnet::Caller::client,"alice",1},request,done);
    owner="bob"; until(runtime,[&] { return result.has_value(); });
    CHECK(result->status==fsnet::Status::rejected && result->data.empty());
    runtime.bind_stateful_context("counter.identity","counter","initial",{true,false,false},
        [](const fsnet::CallIdentity& who,std::string,fsnet::Reply reply) {
            reply({fsnet::Status::ok,fsnet::state_message("initial",who.subject.empty()?"anonymous":"authenticated")});
        });
    auto collision=fsnet::entity_message("42","","same-request"); result.reset();
    parent->contexts.at("counter.identity")({fsnet::Caller::client,"",7},collision,done);
    until(runtime,[&] { return result.has_value(); }); CHECK(result->data=="anonymous"); result.reset();
    parent->contexts.at("counter.identity")({fsnet::Caller::client,"anonymous:context/boot:7",8},collision,done);
    until(runtime,[&] { return result.has_value(); }); CHECK(result->data=="authenticated");
    std::cout << "PASS ownership rechecks: suspended commit and queued private receipt replay reject revoked owners\n";
}
int main() {
    try { ownership(); fencing(); receipts(); ticks_and_limits(); owner_recheck(); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}

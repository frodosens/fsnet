#include "fsnet/attribute_replication.hpp"
#include "fsnet/client.hpp"
#include "fsnet/entity_message.hpp"
#include "fsnet/entity_router.hpp"
#include <iostream>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace fsnet;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

namespace {
void require(bool value, const char* detail) {
    if (!value) throw std::runtime_error(detail);
}

template<class Pump, class Done>
void until(Pump&& pump, Done&& done, std::chrono::milliseconds timeout = 5s) {
    auto deadline = Clock::now() + timeout;
    while (!done()) {
        pump();
        require(Clock::now() < deadline, "production client/router test deadline");
        std::this_thread::sleep_for(1ms);
    }
}

void router_deadlines() {
    Node owner("production-router"), caller("production-router");
    auto db = memory_database();
    EntityCatalog catalog(*db);
    auto id = catalog.create("production-router", "actor", {"Actor", {}, "", "owner"});
    struct Delayed { Clock::time_point due; Reply reply; std::string value; };
    std::vector<Delayed> delayed;
    std::unordered_map<std::string, unsigned> executions;
    owner.bind_rpc("@", {true, true, false}, [&](const CallIdentity& who, std::string input, Reply reply) {
        auto call = parse_entity_call_view(input);
        require(call.has_value(), "valid router fixture envelope");
        auto value = std::string(call->payload);
        auto count = ++executions[value];
        if (call->method == "drop" && count == 1) {
            owner.close(who.peer);
            return;
        }
        if (call->method == "stall") return;
        if (call->method == "drop") return reply({Status::ok, value});
        delayed.push_back({Clock::now() + (call->method == "slow" ? 1500ms : 350ms),
                           std::move(reply), std::move(value)});
    });
    auto port = owner.listen("127.0.0.1", 0);
    EntityRouter router(caller, db, {{"owner", "127.0.0.1", port, {"Actor"}}});
    // Above the transport deadline, keyed requests used to retry local
    // rejection until the remaining time fell below 30s. Reject at admission,
    // with no database/network work or retained queue capacity.
    for (auto timeout : {0ms, -1ms, 30001ms, 300000ms}) {
        for (auto key : {"", "deadline-receipt"}) {
            unsigned completions = 0;
            router.call(id, "medium", "invalid-timeout", timeout, [&](Result result) {
                require(result.status == Status::rejected, "invalid router deadline rejected");
                ++completions;
            }, key);
            require(completions == 1 && router.pending() == 0, "invalid router call rejected before admission");
        }
        unsigned completions = 0;
        router.bind_owner("Actor", id.str(), {}, timeout, [&](Result result) {
            require(result.status == Status::rejected, "invalid owner deadline rejected");
            ++completions;
        });
        require(completions == 1 && router.pending() == 0, "invalid owner call rejected before admission");
    }
    auto pump = [&] {
        owner.poll(); caller.poll(); router.poll();
        for (std::size_t i = 0; i < delayed.size();) {
            if (Clock::now() < delayed[i].due) { ++i; continue; }
            auto operation = std::move(delayed[i]);
            delayed.erase(delayed.begin() + i);
            operation.reply({Status::ok, std::move(operation.value)});
        }
    };
    auto call = [&](std::string_view method, std::string_view value, std::string_view request_id = {}) {
        std::optional<Result> result;
        router.call(id, method, value, 3s, [&](Result response) { result = std::move(response); }, request_id);
        until(pump, [&] { return result.has_value(); }, 4s);
        return *result;
    };
    std::optional<Result> boundary;
    router.call(id, "medium", "maximum-timeout", 30s,
        [&](Result result) { boundary = std::move(result); });
    until(pump, [&] { return boundary.has_value(); });
    require(boundary->status == Status::ok && executions["maximum-timeout"] == 1,
            "maximum supported router deadline executes normally");
    require(call("slow", "unkeyed").status == Status::ok, "unkeyed RPC may use a three-second deadline");
    require(executions["unkeyed"] == 1, "unkeyed slow RPC executes once");
    require(call("slow", "keyed", "slow-receipt").status == Status::ok, "keyed slow RPC succeeds");
    require(executions["keyed"] == 1, "healthy keyed RPC is not retried each second");

    std::optional<Result> short_result, concurrent_result;
    unsigned short_completions = 0;
    router.call(id, "stall", "short", 100ms, [&](Result response) {
        ++short_completions; short_result = std::move(response);
    });
    router.call(id, "medium", "concurrent", 2s, [&](Result response) { concurrent_result = std::move(response); });
    until(pump, [&] { return short_result.has_value() && concurrent_result.has_value(); });
    require(short_result->status == Status::timeout && short_completions == 1, "short RPC completes once at deadline");
    require(concurrent_result->status == Status::ok, "one timeout does not close another RPC's shared connection");
    require(executions["short"] == 1 && executions["concurrent"] == 1, "unkeyed calls are not replayed after timeout");

    require(call("drop", "unkeyed-disconnect").status == Status::disconnected, "unkeyed disconnect is returned");
    auto backoff = Clock::now() + 300ms;
    until(pump, [&] { return Clock::now() >= backoff; });
    require(executions["unkeyed-disconnect"] == 1, "unkeyed possibly executed RPC is never replayed");
    require(call("drop", "keyed-disconnect", "safe-receipt").status == Status::ok, "keyed disconnect may retry within total deadline");
    require(executions["keyed-disconnect"] == 2, "keyed disconnect performs one failure retry");
    router.stop(); caller.stop(); owner.stop();
}

void client_projection_selection() {
    Node server("production-client");
    std::uint64_t accepted = 0;
    server.identify([](auto, const auto&) -> std::optional<CallIdentity> {
        return CallIdentity{Caller::client, "same-account"};
    });
    server.bind_rpc("system.test.peer", {true, false, false}, [&](const CallIdentity& who, std::string, Reply reply) {
        accepted = who.peer; reply({Status::ok, {}});
    });
    auto db = memory_database();
    auto host = std::make_shared<EntityScriptHost>(std::make_shared<NodeScriptHost>(server), db, "owner/boot");
    EntitySchema schema;
    schema.attributes.emplace("x", AttributeDefinition{AttributeType::integer, std::int64_t(1)});
    host->define_schema("Actor", schema);
    auto initial = encode_attributes({0, 0, true, {{"x", std::int64_t(1)}}});
    host->bind_stateful_context("Actor.noop", "Actor", initial, {false, true, false},
        [](const CallIdentity&, std::string input, Reply reply) {
            auto call = parse_entity_message_view(input);
            auto state = parse_state_message_view(call->payload);
            reply({Status::ok, state_message(state->state, "")});
        });
    ReplicationLimits limits;
    limits.per_peer = 256;
    AttributeReplication replication(server, host, {}, limits);
    ClientOptions options;
    options.token = "production-client";
    Client client(options);
    auto port = server.listen("127.0.0.1", 0);
    auto pump = [&] { server.poll(); client.poll(); host->poll(); replication.poll(); };
    auto wait = [&](auto begin) {
        std::optional<Result> result;
        begin([&](Result response) { result = std::move(response); });
        until(pump, [&] { return result.has_value(); });
        return *result;
    };
    client.connect({"127.0.0.1", port, {}});
    until(pump, [&] { return client.state() == ClientState::connected; });
    require(wait([&](Reply reply) { client.request("system.test.peer", "", std::move(reply)); }).status == Status::ok, "capture SDK fixture peer");
    auto connection = server.identity(accepted)->connection;
    auto create = [&](std::string key) {
        auto result = wait([&](Reply reply) {
            host->create_entity("Actor", {}, {"production-client", key, connection, "owner"}, std::move(reply));
        });
        require(result.status == Status::ok, "create owned client fixture entity");
        return EntityId(result.data);
    };
    auto watch = [&](EntityId id) {
        std::optional<Status> status;
        ClientEntityHandle handle;
        client.watch({"Actor", id.str()}, [&](Status response, ClientEntityHandle result) { status = response; handle = result; });
        until(pump, [&] { return status.has_value(); });
        require(*status == Status::ok && bool(handle), "manual watch succeeds");
        return handle;
    };
    auto visibility = [&](EntityId id, unsigned action) {
        require(wait([&](Reply reply) { host->entity_visibility(accepted, "Actor", id.str(), action, std::move(reply)); }).status == Status::ok,
                "trusted visibility action succeeds");
    };
    auto automatic = [&](EntityId id) {
        auto view = client.entity(id), keyed = client.entity(EntityKey{"Actor", id.str()});
        return view && keyed && view->automatic && keyed->automatic && view->owned == true &&
               view->controlled && keyed->handle == view->handle;
    };
    auto before = create("manual-first");
    auto before_manual = watch(before);
    visibility(before, 1); visibility(before, 3);
    until(pump, [&] { return automatic(before); });
    require(!client.entity(before_manual)->automatic, "manual handle continues addressing its own projection");
    auto after = create("automatic-first");
    visibility(after, 1); visibility(after, 3);
    until(pump, [&] { return automatic(after); });
    auto after_manual = watch(after);
    require(automatic(after), "later manual watch cannot replace automatic ownership/control lookup");
    require(!client.entity(after_manual)->automatic, "manual and automatic handles remain distinct");
    require(!client.entity(EntityKey{"Wrong", after.str()}) && !client.entity(EntityKey{"", after.str()}), "EntityKey still requires an exact type");

    // Grow bindings through several unordered_map rehashes and check that the
    // stable ID/key selection does not depend on insertion or hash iteration.
    for (unsigned i = 0; i < 72; ++i) {
        auto id = create("extra-" + std::to_string(i));
        watch(id); visibility(id, 1);
    }
    until(pump, [&] { return client.stats().entities >= 148; });
    require(automatic(after), "rehash preserves authoritative automatic selection");
    auto later_manual = watch(after);
    visibility(after, 2);
    until(pump, [&] { auto view = client.entity(after); return view && !view->automatic; });
    require(client.entity(after)->handle == after_manual && client.entity(EntityKey{"Actor", after.str()})->handle == after_manual,
            "duplicate manual lookup deterministically chooses oldest live subscription");
    client.unwatch(after_manual);
    require(client.entity(after)->handle == later_manual, "removing preferred manual projection selects the remaining watch");
    client.disconnect(); replication.stop(); host->stop(); server.stop();
}
}

int main() {
    try {
        router_deadlines();
        client_projection_selection();
        std::cout << "PASS production client/router deadlines, retry semantics and deterministic projections\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

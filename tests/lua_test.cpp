#include "fsnet/lua_engine.hpp"
#include "fsnet/entity_message.hpp"
#include "fsnet/entity_host.hpp"
#include "fsnet/entity_router.hpp"
#include <chrono>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <thread>
#include <unordered_map>

using namespace std::chrono_literals;
#define CHECK(condition) do { if (!(condition)) throw std::runtime_error(std::string(__FILE__) + ":" + std::to_string(__LINE__) + " " #condition); } while (false)
struct Host final : fsnet::ScriptHost {
    std::unordered_map<std::string, fsnet::Handler> handlers;
    fsnet::Reply delayed;
    bool has_route(const std::string& route) const override { return handlers.contains(route); }
    void bind(std::string route, fsnet::Handler handler) override {
        if (!handlers.emplace(std::move(route), std::move(handler)).second) throw std::runtime_error("duplicate");
    }
    void unbind(const std::string& route) override { handlers.erase(route); }
    std::uint64_t connect(const std::string&, std::uint16_t) override { return 42; }
    void call(std::uint64_t peer, std::string route, std::string data, std::chrono::milliseconds, fsnet::Reply reply) override {
        CHECK(peer == 42);
        if (route == "hang") delayed = std::move(reply);
        else if (route == "player.read") { CHECK(data == fsnet::entity_message("42", "binary")); reply({fsnet::Status::ok, "encoded"}); }
        else if (route == "echo") reply({fsnet::Status::ok, std::move(data)});
        else if (route == "double") { reply({fsnet::Status::ok, data}); reply({fsnet::Status::internal, {}}); }
        else reply({fsnet::Status::timeout, {}});
    }
};
fsnet::Result invoke(Host& host, fsnet::LuaEngine& engine, const std::string& route, std::string input) {
    std::optional<fsnet::Result> result;
    host.handlers.at(route)(std::move(input), [&](fsnet::Result reply) { result = std::move(reply); });
    for (int i = 0; !result && i < 100; ++i) engine.poll();
    CHECK(result); return *result;
}
void lifecycle() {
    auto host = std::make_shared<Host>();
    fsnet::LuaEngine engine(host);
    engine.load(R"(
        assert(io == nil and os == nil and debug == nil and package == nil and load == nil)
        runtime.rpc('echo', function(data) return data end)
        runtime.rpc('forward', function(data) return runtime.request(42, 'echo', data) end)
        runtime.rpc('double', function(data) return runtime.request(42, 'double', data) end)
        runtime.rpc('timeout', function(data)
            local value, status = runtime.await(42, 'missing', data, 30)
            assert(status == runtime.status.timeout and value == '')
            return 'handled timeout'
        end)
        runtime.rpc('fail', function() error('script application error') end)
        runtime.rpc('wrong', function() return 123 end)
        runtime.rpc('wait', function(data) return runtime.request(42, 'hang', data) end)
    )");
    auto binary = std::string("hello\0world", 11);
    auto echo = invoke(*host, engine, "echo", binary); CHECK(echo.status == fsnet::Status::ok && echo.data == binary);
    auto forwarded = invoke(*host, engine, "forward", binary); CHECK(forwarded.status == fsnet::Status::ok && forwarded.data == binary);
    CHECK(invoke(*host, engine, "double", binary).data == binary);
    auto timeout = invoke(*host, engine, "timeout", {}); CHECK(timeout.status == fsnet::Status::ok && timeout.data == "handled timeout");
    CHECK(invoke(*host, engine, "fail", {}).status == fsnet::Status::internal);
    CHECK(engine.last_error().find("script application error") != std::string::npos);
    CHECK(invoke(*host, engine, "wrong", {}).status == fsnet::Status::internal);
    std::optional<fsnet::Result> waiting;
    host->handlers.at("wait")("pending", [&](fsnet::Result reply) { waiting = std::move(reply); });
    engine.poll(); CHECK(!waiting && engine.active() == 1);
    engine.stop(); engine.stop();
    CHECK(waiting && waiting->status == fsnet::Status::disconnected);
    CHECK(host->handlers.empty() && engine.active() == 0 && engine.memory_used() == 0);
    host->delayed({fsnet::Status::ok, "late completion"}); CHECK(engine.poll() == 0);
    std::cout << "PASS Lua: binary RPC, immediate async completion, errors, suspension, shutdown and late completion\n";
}
void budgets() {
    auto host = std::make_shared<Host>();
    fsnet::ScriptLimits limits; limits.memory_bytes = 512 * 1024; limits.instructions_per_resume = 10000;
    limits.queued_requests = 1;
    limits.queued_bytes = 16;
    fsnet::LuaEngine engine(host, limits);
    engine.load(R"(
        runtime.rpc('echo', function(data) return data end)
        runtime.rpc('loop', function() while true do pcall(function() while true do end end) end end)
        runtime.rpc('memory', function() return string.rep('x', 2 * 1024 * 1024) end)
    )");
    CHECK(invoke(*host, engine, "loop", {}).status == fsnet::Status::overloaded);
    CHECK(engine.last_error().find("instruction") != std::string::npos);
    CHECK(invoke(*host, engine, "memory", {}).status == fsnet::Status::overloaded);
    CHECK(engine.memory_used() <= limits.memory_bytes);
    CHECK(invoke(*host, engine, "echo", "healthy").data == "healthy");
    std::optional<fsnet::Result> first, overflow;
    host->handlers.at("echo")("one", [&](fsnet::Result result) { first = std::move(result); });
    host->handlers.at("echo")("two", [&](fsnet::Result result) { overflow = std::move(result); });
    CHECK(!first && overflow && overflow->status == fsnet::Status::overloaded);
    engine.poll(); CHECK(first && first->data == "one");
    CHECK(invoke(*host, engine, "echo", std::string(17, 'x')).status == fsnet::Status::overloaded);
    std::cout << "PASS Lua limits: instruction yield cannot be caught by pcall, OOM recovery, bounded queue\n";
}
void failed_load() {
    for (const auto& source : {"runtime.rpc('partial', function(s) return s end); error('load failed')",
                              "runtime.rpc('partial', function(s) return s end); while true do end",
                              "this is not Lua", "runtime.rpc('x', function(s) return s end); runtime.rpc('x', function(s) return s end)"}) {
        auto host = std::make_shared<Host>(); fsnet::LuaEngine engine(host);
        bool failed = false;
        try { engine.load(source); } catch (const std::exception&) { failed = true; }
        CHECK(failed && host->handlers.empty() && engine.memory_used() == 0);
    }
    std::cout << "PASS Lua load: syntax, startup exception, startup loop and duplicate routes leave no partial registrations\n";
}
void task_deadline() {
    auto host = std::make_shared<Host>();
    fsnet::ScriptLimits limits; limits.task_timeout = 5ms;
    fsnet::LuaEngine engine(host, limits);
    engine.load("runtime.rpc('wait', function(s) return runtime.request(42, 'hang', s) end)");
    std::optional<fsnet::Result> result;
    host->handlers.at("wait")("waiting", [&](fsnet::Result reply) { result = std::move(reply); });
    engine.poll(); CHECK(!result && engine.active() == 1);
    std::this_thread::sleep_for(10ms); engine.poll();
    CHECK(result && result->status == fsnet::Status::timeout && engine.active() == 0);
    host->delayed({fsnet::Status::ok, "late"}); engine.poll(); CHECK(engine.active() == 0);
    std::cout << "PASS Lua task deadline: suspended task expires and ignores its late completion\n";
}
void entity_serialization() {
    auto host = std::make_shared<Host>();
    fsnet::ScriptLimits limits; limits.task_timeout = 100ms;
    fsnet::LuaEngine engine(host, limits);
    engine.load(R"(
        local values = {}
        runtime.entity_rpc('player', 'wait', function(id, data)
            values[id] = 'started'
            local reply = runtime.request(42, 'hang', data)
            values[id] = reply
            return reply
        end)
        runtime.entity_rpc('player', 'read', function(id) return values[id] or 'empty' end)
        runtime.entity_rpc('player', 'mark', function(id, data) values[id] = (values[id] or '') .. data; return values[id] end)
        runtime.entity_rpc('player', 'fail', function() error('failed') end)
        runtime.rpc('encode', function(data) return runtime.entity_request(42, 'player', '42', 'read', data) end)
    )");
    std::optional<fsnet::Result> first, second, third, other;
    auto submit = [&](const char* method, const char* id, std::optional<fsnet::Result>& result) {
        host->handlers.at(std::string("player.") + method)(fsnet::entity_message(id, "payload"),
            [&result](fsnet::Result reply) { result = std::move(reply); });
    };
    submit("wait", "42", first); engine.poll(); CHECK(!first);
    std::string order;
    for (const auto* suffix : {"a", "b"}) host->handlers.at("player.mark")(fsnet::entity_message("42", suffix),
        [&](fsnet::Result result) { CHECK(result.status == fsnet::Status::ok); order += result.data + ";"; });
    submit("read", "42", second); submit("read", "43", other);
    engine.poll(); CHECK(!second && other && other->data == "empty");
    host->delayed({fsnet::Status::ok, "finished"}); engine.poll();
    CHECK(first && first->data == "finished" && second && second->data == "finishedab");
    CHECK(order == "finisheda;finishedab;");
    first.reset(); second.reset();
    submit("fail", "42", first); submit("read", "42", second); engine.poll();
    CHECK(first && first->status == fsnet::Status::internal && second && second->data == "finishedab");
    first.reset(); second.reset();
    submit("wait", "42", first); engine.poll(); auto late = host->delayed;
    std::this_thread::sleep_for(110ms);
    submit("read", "42", second); engine.poll();
    CHECK(first && first->status == fsnet::Status::timeout && second && second->data == "started");
    late({fsnet::Status::ok, "stale"}); engine.poll();
    submit("read", "42", third); engine.poll(); CHECK(third && third->data == "started");
    CHECK(invoke(*host, engine, "player.read", "invalid").status == fsnet::Status::rejected);
    // Exercise the Lua encoder against the independently implemented native encoder.
    host->handlers.at("encode")("binary", [&](fsnet::Result result) {
        CHECK(result.status == fsnet::Status::ok && result.data == "encoded");
    });
    engine.poll();
    first.reset(); second.reset();
    submit("wait", "42", first); engine.poll(); submit("read", "42", second);
    engine.stop(); CHECK(first && second && first->status == fsnet::Status::disconnected && second->status == fsnet::Status::disconnected);
    std::cout << "PASS entity mailbox: FIFO across methods, independent entities, await, failure, timeout, late reply and stop\n";
}
void persistent_lua() {
    auto db = fsnet::memory_database();
    auto parent = std::make_shared<Host>();
    auto host = std::make_shared<fsnet::EntityScriptHost>(parent, db, "lua/boot-1");
    fsnet::LuaEngine engine(host);
    const auto source = R"(
        runtime.entity('counter', {
            initial = '0',
            methods = {
                add = function(state, id, payload)
                    assert(id == '42')
                    local next = tostring(tonumber(state) + tonumber(runtime.request(42, 'echo', payload)))
                    return next, next
                end,
                get = function(state) return state, state end,
                fail = function(state) error('discard tentative state') end,
            }
        })
    )";
    engine.load(source);
    auto call = [&](const char* method, const char* payload) {
        std::optional<fsnet::Result> result;
        parent->handlers.at(std::string("counter.") + method)(fsnet::entity_message("42", payload),
            [&](fsnet::Result reply) { result = std::move(reply); });
        auto deadline = std::chrono::steady_clock::now() + 2s;
        while (!result) { host->poll(); engine.poll(); CHECK(std::chrono::steady_clock::now() < deadline); std::this_thread::sleep_for(1ms); }
        return *result;
    };
    CHECK(call("add", "7").data == "7"); CHECK(call("fail", "").status == fsnet::Status::internal);
    CHECK(call("get", "").data == "7");
    auto handle = host->handle("counter", "42"); CHECK(handle);
    std::optional<fsnet::Result> deactivated;
    host->deactivate(*handle, [&](fsnet::Result result) { deactivated = std::move(result); });
    while (!deactivated) { host->poll(); std::this_thread::sleep_for(1ms); }
    CHECK(deactivated->status == fsnet::Status::ok);
    engine.stop(); host->stop();
    // Replace the VM and its host, restoring solely from committed database bytes.
    auto recovered_parent = std::make_shared<Host>();
    auto recovered = std::make_shared<fsnet::EntityScriptHost>(recovered_parent, db, "lua/boot-2");
    fsnet::LuaEngine new_engine(recovered); new_engine.load(source);
    std::optional<fsnet::Result> result;
    recovered_parent->handlers.at("counter.get")(fsnet::entity_message("42", ""), [&](fsnet::Result reply) { result = std::move(reply); });
    auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!result) { recovered->poll(); new_engine.poll(); CHECK(std::chrono::steady_clock::now() < deadline); std::this_thread::sleep_for(1ms); }
    CHECK(result->status == fsnet::Status::ok && result->data == "7");
    CHECK(recovered->stats().committed == 1);
    std::cout << "PASS Lua stateful entities: async RPC, committed state, failure isolation and VM replacement recovery\n";
}
void callback_shutdown() {
    auto host = std::make_shared<Host>();
    auto engine = std::make_unique<fsnet::LuaEngine>(host);
    engine->load("runtime.rpc('echo', function(s) return s end)");
    bool completed = false;
    host->handlers.at("echo")("done", [&](fsnet::Result result) {
        CHECK(result.status == fsnet::Status::ok && result.data == "done");
        completed = true; engine.reset();
    });
    auto* running = engine.get(); running->poll();
    CHECK(completed && !engine && host->handlers.empty());
    std::cout << "PASS Lua lifetime: engine destruction from completion is deferred until VM execution exits\n";
}
void lua_routing() {
    fsnet::Node server("token"), gateway("token"), client("token");
    auto db = fsnet::memory_database();
    auto transport = std::make_shared<fsnet::NodeScriptHost>(server);
    auto entities = std::make_shared<fsnet::EntityScriptHost>(transport, db, "server/boot-1");
    fsnet::LuaEngine backend(entities);
    backend.load("runtime.entity('counter', {initial='0', methods={add=function(state, id, data) local next=tostring(tonumber(state)+tonumber(data)); return next, next end}})");
    fsnet::EntityRouter router(gateway, db, {{"server", "127.0.0.1", server.listen("127.0.0.1", 0), {"counter"}}});
    auto gateway_transport = std::make_shared<fsnet::NodeScriptHost>(gateway);
    auto routed = std::make_shared<fsnet::RoutedScriptHost>(gateway_transport, router);
    fsnet::LuaEngine frontend(routed);
    frontend.load("runtime.rpc('gateway.add', function(data) return runtime.entity_call('counter', '42', 'add', data, 1000, 'request-1') end)");
    auto peer = client.connect("127.0.0.1", gateway.listen("127.0.0.1", 0));
    auto call = [&] {
        std::optional<fsnet::Result> result;
        client.call(peer, "gateway.add", "7", 2s, [&](fsnet::Result reply) { result = std::move(reply); });
        auto deadline = std::chrono::steady_clock::now() + 3s;
        while (!result) {
            server.poll(); gateway.poll(); client.poll(); entities->poll(); router.poll(); backend.poll(); frontend.poll();
            CHECK(std::chrono::steady_clock::now() < deadline); std::this_thread::sleep_for(1ms);
        }
        CHECK(result->status == fsnet::Status::ok && result->data == "7");
    };
    call(); call(); CHECK(entities->stats().replayed == 1);
    std::cout << "PASS Lua routed entity call: gateway coroutine -> owner lookup -> state commit -> duplicate receipt replay\n";
}
void tcp() {
    fsnet::Node server("lua-test-token"), client("lua-test-token"), backend("lua-test-token");
    backend.bind("echo", [](std::string data, fsnet::Reply reply) { reply({fsnet::Status::ok, std::move(data)}); });
    auto port = backend.listen("127.0.0.1", 0);
    auto host = std::make_shared<fsnet::NodeScriptHost>(server); fsnet::LuaEngine engine(host);
    engine.load("local peer; runtime.rpc('forward', function(data) peer = peer or runtime.connect('127.0.0.1', " +
                std::to_string(port) + "); return runtime.request(peer, 'echo', data, 1000) end)");
    auto peer = client.connect("127.0.0.1", server.listen("127.0.0.1", 0));
    std::optional<fsnet::Result> result;
    client.call(peer, "forward", "over TCP", 2s, [&](fsnet::Result reply) { result = std::move(reply); });
    auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!result) {
        server.poll(); backend.poll(); client.poll(); engine.poll();
        CHECK(std::chrono::steady_clock::now() < deadline); std::this_thread::sleep_for(1ms);
    }
    CHECK(result->status == fsnet::Status::ok && result->data == "over TCP");
    engine.stop(); CHECK(!server.has_route("forward") && server.has_route("system.health"));
    std::cout << "PASS Lua host: real TCP client -> Lua coroutine -> backend -> resumed Lua reply\n";
}
void rpc_policies() {
    fsnet::Node server("policy-token"), client("policy-token");
    auto transport = std::make_shared<fsnet::NodeScriptHost>(server);
    auto entities = std::make_shared<fsnet::EntityScriptHost>(transport, fsnet::memory_database(), "policy/boot");
    fsnet::LuaEngine engine(entities);
    engine.load(R"(
        runtime.entity('counter', {initial='0', methods={
            read={callers={'client'}, handler=function(state) return state, state end},
            increment={callers={'server'}, handler=function(state) local n=tostring(tonumber(state)+1); return n,n end}
        }})
    )");
    auto peer = client.connect("127.0.0.1", server.listen("127.0.0.1", 0));
    auto invoke = [&](const char* method) {
        std::optional<fsnet::Result> result;
        client.call(peer, std::string("counter.") + method, fsnet::entity_message("42", ""), 1s,
            [&](fsnet::Result value) { result = std::move(value); });
        auto deadline = std::chrono::steady_clock::now() + 2s;
        while (!result) {
            server.poll(); client.poll(); entities->poll(); engine.poll();
            CHECK(std::chrono::steady_clock::now() < deadline); std::this_thread::sleep_for(1ms);
        }
        return *result;
    };
    CHECK(invoke("increment").status == fsnet::Status::rejected);
    CHECK(invoke("read").data == "0");
    server.identify([](std::uint64_t, const std::string&) { return std::optional(fsnet::CallIdentity{fsnet::Caller::server, "test-service"}); });
    CHECK(invoke("increment").data == "1");
    CHECK(invoke("read").status == fsnet::Status::rejected);
    engine.stop();
    for (auto definition : {"{callers={},handler=function()end}", "{callers={'bogus'},handler=function()end}",
                            "{callers={'client'},access='owner',handler=function()end}"}) {
        fsnet::LuaEngine invalid(entities); bool rejected = false;
        try { invalid.load(std::string("runtime.entity('invalid', {methods={read=") + definition + "}})"); }
        catch (const std::exception&) { rejected = true; }
        CHECK(rejected && !server.has_route("invalid.read"));
    }
    std::cout << "PASS Lua explicit RPC caller policies through persistent host and TCP transport, invalid policy rollback\n";
}
int main() {
    try { lifecycle(); budgets(); failed_load(); task_deadline(); callback_shutdown(); entity_serialization(); persistent_lua(); lua_routing(); tcp(); rpc_policies(); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}

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
    std::unordered_map<std::string, fsnet::ContextHandler> contexts;
    void bind_context(std::string route, fsnet::RpcPolicy, fsnet::ContextHandler handler) override { contexts.emplace(std::move(route), std::move(handler)); }
    bool has_route(const std::string& route) const override { return handlers.contains(route); }
    void bind(std::string route, fsnet::Handler handler) override {
        if (!handlers.emplace(std::move(route), std::move(handler)).second) throw std::runtime_error("duplicate");
    }
    void unbind(const std::string& route) override { handlers.erase(route); contexts.erase(route); }
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
        runtime.legacy_rpc('echo', function(data) return data end)
        runtime.legacy_rpc('forward', function(data) return runtime.request(42, 'echo', data) end)
        runtime.legacy_rpc('double', function(data) return runtime.request(42, 'double', data) end)
        runtime.legacy_rpc('timeout', function(data)
            local value, status = runtime.await(42, 'missing', data, 30)
            assert(status == runtime.status.timeout and value == '')
            return 'handled timeout'
        end)
        runtime.legacy_rpc('fail', function() error('script application error') end)
        runtime.legacy_rpc('wrong', function() return 123 end)
        runtime.legacy_rpc('wait', function(data) return runtime.request(42, 'hang', data) end)
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
        runtime.legacy_rpc('echo', function(data) return data end)
        runtime.legacy_rpc('loop', function() while true do pcall(function() while true do end end) end end)
        runtime.legacy_rpc('memory', function() return string.rep('x', 2 * 1024 * 1024) end)
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
    for (const auto& source : {"runtime.legacy_rpc('partial', function(s) return s end); error('load failed')",
                              "runtime.legacy_rpc('partial', function(s) return s end); while true do end",
                              "this is not Lua", "runtime.legacy_rpc('x', function(s) return s end); runtime.legacy_rpc('x', function(s) return s end)"}) {
        auto host = std::make_shared<Host>(); fsnet::LuaEngine engine(host);
        bool failed = false;
        try { engine.load(source); } catch (const std::exception&) { failed = true; }
        CHECK(failed && host->handlers.empty() && engine.memory_used() == 0);
    }
    std::cout << "PASS Lua load: syntax, startup exception, startup loop and duplicate routes leave no partial registrations\n";
}
void pattern_budgets() {
    auto host = std::make_shared<Host>();
    fsnet::ScriptLimits limits; limits.instructions_per_resume = 1000;
    fsnet::LuaEngine engine(host, limits);
    engine.load(R"(
        runtime.legacy_rpc('find', function(s) return tostring(s:find('a*a*a*a*b')) end)
        runtime.legacy_rpc('match', function(s) return tostring(s:match('a*a*a*a*b')) end)
        runtime.legacy_rpc('gmatch', function(s)
            for _ in s:gmatch('a*a*a*a*b') do end
            return ''
        end)
        runtime.legacy_rpc('gsub', function(s) return (s:gsub('a*a*a*a*b', 'c')) end)
        runtime.legacy_rpc('catch', function(s)
            pcall(string.find, s, 'a*a*a*a*b')
            return 'caught'
        end)
        runtime.legacy_rpc('catch_loop', function(s)
            while true do pcall(string.find, s, 'a*a*a*a*b') end
        end)
        runtime.legacy_rpc('healthy', function(s)
            local a,b = s:match('^(%a+):(%d+)$')
            assert(a == 'item' and b == '42')
            assert(s:find('42', 1, true) == 6)
            return (s:gsub('%d+', '7'))
        end)
    )");
    for (const auto* method : {"find", "match", "gmatch", "gsub", "catch", "catch_loop"}) {
        const auto start = std::chrono::steady_clock::now();
        CHECK(invoke(*host, engine, method, std::string(200, 'a')).status == fsnet::Status::overloaded);
        // The original C matcher takes seconds/minutes for this input. Keep a
        // broad wall limit so this checks interruption without timing tiny ops.
        CHECK(std::chrono::steady_clock::now() - start < 2s);
    }
    CHECK(invoke(*host, engine, "healthy", "item:42").data == "item:7");
    auto deadline_host = std::make_shared<Host>();
    limits.instructions_per_resume = 1000000000; limits.task_timeout = 1ms;
    fsnet::LuaEngine deadline_engine(deadline_host, limits);
    deadline_engine.load(R"(
        runtime.legacy_rpc('slow',function(s) pcall(string.find,s,'a*a*a*a*b');return '' end)
        runtime.legacy_rpc('slow_loop',function(s) while true do pcall(string.find,s,'a*a*a*a*b') end end)
    )");
    CHECK(invoke(*deadline_host, deadline_engine, "slow", std::string(200, 'a')).status == fsnet::Status::timeout);
    CHECK(invoke(*deadline_host, deadline_engine, "slow_loop", std::string(200, 'a')).status == fsnet::Status::timeout);
    std::cout << "PASS Lua patterns: bounded C backtracking, gmatch/gsub, pcall, deadline and recovery\n";
}
void task_deadline() {
    auto host = std::make_shared<Host>();
    fsnet::ScriptLimits limits; limits.task_timeout = 5ms;
    fsnet::LuaEngine engine(host, limits);
    engine.load("runtime.legacy_rpc('wait', function(s) return runtime.request(42, 'hang', s) end)");
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
        runtime.legacy_entity_rpc('player', 'wait', function(id, data)
            values[id] = 'started'
            local reply = runtime.request(42, 'hang', data)
            values[id] = reply
            return reply
        end)
        runtime.legacy_entity_rpc('player', 'read', function(id) return values[id] or 'empty' end)
        runtime.legacy_entity_rpc('player', 'mark', function(id, data) values[id] = (values[id] or '') .. data; return values[id] end)
        runtime.legacy_entity_rpc('player', 'fail', function() error('failed') end)
        runtime.legacy_rpc('encode', function(data) return runtime.entity_request(42, 'player', '42', 'read', data) end)
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
        runtime.legacy_entity('counter', {
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
    engine->load("runtime.legacy_rpc('echo', function(s) return s end)");
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
    backend.load("runtime.legacy_entity('counter', {initial='0', methods={add=function(state, id, data) local next=tostring(tonumber(state)+tonumber(data)); return next, next end}})");
    fsnet::EntityRouter router(gateway, db, {{"server", "127.0.0.1", server.listen("127.0.0.1", 0), {"counter"}}});
    auto gateway_transport = std::make_shared<fsnet::NodeScriptHost>(gateway);
    auto routed = std::make_shared<fsnet::RoutedScriptHost>(gateway_transport, router);
    fsnet::LuaEngine frontend(routed);
    frontend.load("runtime.legacy_rpc('gateway.add', function(data) return runtime.entity_call('counter', '42', 'add', data, 1000, 'request-1') end)");
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
    engine.load("local peer; runtime.legacy_rpc('forward', function(data) peer = peer or runtime.connect('127.0.0.1', " +
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
void context_suspension() {
    auto host = std::make_shared<Host>(); fsnet::LuaEngine engine(host);
    engine.load(R"(
        runtime.legacy_rpc('identity', {callers={'client'}, handler=function(ctx, data)
            if data == 'wait' then runtime.request(42, 'hang', data) end
            return ctx.subject
        end})
    )");
    std::optional<fsnet::Result> first, second;
    fsnet::CallIdentity who{fsnet::Caller::client, "alice", 1};
    host->contexts.at("identity")(who, "wait", [&](fsnet::Result r) { first = std::move(r); });
    engine.poll(); CHECK(!first && host->delayed);
    who.subject = "bob"; who.peer = 2;
    host->contexts.at("identity")(who, "now", [&](fsnet::Result r) { second = std::move(r); });
    engine.poll(); CHECK(second && second->data == "bob" && !first);
    host->delayed({fsnet::Status::ok, "resume"}); engine.poll();
    CHECK(first && first->data == "alice");
    std::cout << "PASS Lua request context survives suspension and another concurrent principal\n";
}
void rpc_policies() {
    fsnet::Node server("policy-token"), client("policy-token");
    auto transport = std::make_shared<fsnet::NodeScriptHost>(server);
    auto entities = std::make_shared<fsnet::EntityScriptHost>(transport, fsnet::memory_database(), "policy/boot");
    fsnet::LuaEngine engine(entities);
    engine.load(R"(
        runtime.legacy_entity('counter', {initial='0', methods={
            read={callers={'client'}, handler=function(state) return state, state end},
            owned={callers={'client'}, access='owner', handler=function(state, id, data, ctx)
                assert(ctx.caller == 'client' and ctx.subject == 'alice' and ctx.peer > 0)
                return state, ctx.subject
            end},
            identify={callers={'client'}, handler=function(state, id, data, ctx)
                local n=tostring(tonumber(state)+1); return n,ctx.subject..':'..n
            end},
            increment={callers={'server'}, handler=function(state) local n=tostring(tonumber(state)+1); return n,n end}
        }})
    )");
    auto peer = client.connect("127.0.0.1", server.listen("127.0.0.1", 0));
    auto invoke = [&](const char* method, const std::string& request_id = "") {
        std::optional<fsnet::Result> result;
        client.call(peer, std::string("counter.") + method, fsnet::entity_message("42", "", request_id), 1s,
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
    CHECK(invoke("owned").status == fsnet::Status::rejected); // no owner resolver: fail closed
    entities->legacy_ownership([](const std::string& type, const std::string&) { CHECK(type == "counter"); return "alice"; });
    fsnet::CallIdentity who{fsnet::Caller::client, "alice"};
    server.identify([&](std::uint64_t, const std::string&) { return std::optional(who); });
    CHECK(invoke("owned").data == "alice");
    CHECK(invoke("identify", "same-request").data == "alice:1");
    CHECK(invoke("identify", "same-request").data == "alice:1"); // retry receipt
    who.subject = "bob";
    CHECK(invoke("owned").status == fsnet::Status::rejected);
    CHECK(invoke("identify", "same-request").data == "bob:2"); // never replay Alice's response
    entities->legacy_ownership([](const std::string&, const std::string&) -> std::string { throw std::runtime_error("mapping error"); });
    CHECK(invoke("owned").status == fsnet::Status::rejected);
    server.identify([](std::uint64_t, const std::string&) { return std::optional(fsnet::CallIdentity{fsnet::Caller::server, "test-service"}); });
    CHECK(invoke("increment").data == "3");
    CHECK(invoke("read").status == fsnet::Status::rejected);
    engine.stop();
    for (auto definition : {"{callers={},handler=function()end}", "{callers={'bogus'},handler=function()end}",
                            "{callers={'client'},access='invalid',handler=function()end}"}) {
        fsnet::LuaEngine invalid(entities); bool rejected = false;
        try { invalid.load(std::string("runtime.legacy_entity('invalid', {methods={read=") + definition + "}})"); }
        catch (const std::exception&) { rejected = true; }
        CHECK(rejected && !server.has_route("invalid.read"));
    }
    std::cout << "PASS Lua explicit RPC caller policies through persistent host and TCP transport, invalid policy rollback\n";
}
int main() {
    try { lifecycle(); budgets(); failed_load(); pattern_budgets(); task_deadline(); callback_shutdown(); entity_serialization(); persistent_lua(); lua_routing(); tcp(); rpc_policies(); context_suspension(); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}

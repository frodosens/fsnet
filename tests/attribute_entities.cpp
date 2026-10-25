#include "fsnet/lua_engine.hpp"
#include "fsnet/entity_host.hpp"
#include "fsnet/entity_message.hpp"
#include "fsnet/entity_record.hpp"
#include "fsnet/attribute_wire.hpp"
#include <chrono>
#include <filesystem>
#include <iostream>
#include <optional>
#include <thread>
using namespace std::chrono_literals;
#define CHECK(x) do { if (!(x)) throw std::runtime_error(std::string(__FILE__) + ':' + std::to_string(__LINE__) + " " #x); } while (false)
struct FaultStorage final : fsnet::Storage {
    std::shared_ptr<fsnet::Storage> underlying;
    bool fail = false;
    explicit FaultStorage(std::shared_ptr<fsnet::Storage> storage) : underlying(std::move(storage)) {}
    std::optional<fsnet::Record> get(std::string_view k) override { return underlying->get(k); }
    bool compare_exchange(std::string_view k, std::uint64_t v, std::string_view s) override { return !fail && underlying->compare_exchange(k, v, s); }
    bool erase(std::string_view k, std::uint64_t v) override { return underlying->erase(k, v); }
};
int main() {
    namespace fs = std::filesystem;
    auto directory = fs::temp_directory_path() / ("fsnet-attrs-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        auto storage = std::make_shared<FaultStorage>(fsnet::file_storage(directory.string()));
        fsnet::Node server("test"), client("test");
        auto transport = std::make_shared<fsnet::NodeScriptHost>(server);
        auto entities = std::make_shared<fsnet::EntityScriptHost>(transport, storage, "attrs/boot-1");
        entities->legacy_ownership([](const std::string&, const std::string&) { return "alice"; });
        std::vector<fsnet::EntityScriptHost::AttributeCommit> events;
        entities->on_attributes([&](const auto& event) { events.push_back(event); });
        fsnet::CallIdentity who{fsnet::Caller::client, "alice"};
        server.identify([&](std::uint64_t, const std::string&) { return std::optional(who); });
        fsnet::LuaEngine engine(entities);
        engine.load(R"(
            runtime.legacy_entity('player', {
                attrs = {
                    hp={type='int32', default=100, server='rw', client='r', sync='observers'},
                    gold={type='int64', default=10, server='rw', client='r', sync='owner'},
                    secret={type='string', default='private', server='rw', client='none', sync='none'},
                    temporary={type='int32', default=0, server='rw', client='r', sync='owner', persist=false},
                },
                methods={
                    read={callers={'client'}, access='owner', handler=function(self, ctx)
                        assert(ctx.subject == 'alice'); return self.hp..':'..self.temporary
                    end},
                    damage={callers={'server'}, handler=function(self, ctx, amount)
                        self.hp=self.hp-tonumber(amount); self.temporary=self.temporary+1; self.gold=self.gold+1
                        return self.hp..':'..self.temporary
                    end},
                    invalid={callers={'server'}, handler=function(self) self.hp='oops'; return '' end},
                    overflow={callers={'server'}, handler=function(self) self.hp=2147483648; return '' end},
                    fail={callers={'server'}, handler=function(self) self.hp=1; error('abort') end},
                },
            })
        )");
        auto peer = client.connect("127.0.0.1", server.listen("127.0.0.1", 0));
        auto call = [&](const char* method, std::string payload = "") {
            std::optional<fsnet::Result> result;
            client.call(peer, std::string("player.") + method, fsnet::entity_message("42", payload), 1s,
                [&](fsnet::Result r) { result = std::move(r); });
            auto end = std::chrono::steady_clock::now() + 2s;
            while (!result) { server.poll(); client.poll(); entities->poll(); engine.poll(); CHECK(std::chrono::steady_clock::now() < end); std::this_thread::sleep_for(1ms); }
            return *result;
        };
        CHECK(call("read").data == "100:0"); CHECK(events.empty());
        CHECK(call("damage", "10").status == fsnet::Status::rejected);
        who = {fsnet::Caller::server, "logic"};
        CHECK(call("damage", "10").data == "90:1"); CHECK(events.size() == 1);
        auto record = storage->get(fsnet::entity_record_key("player", "42")); CHECK(record);
        auto image = fsnet::decode_attributes(fsnet::decode_entity_record(record->value).snapshot.state);
        CHECK(image.values.contains("secret") && !image.values.contains("temporary"));
        CHECK(call("damage", "10").data == "80:2"); CHECK(events.size() == 2);
        fsnet::EntityAttributes projection(*events.back().schema, events.back().owner);
        projection.restore_full(events.back().current.values, events.back().current.revision);
        CHECK(projection.snapshot({"alice", true}).values.size() == 3);
        auto observer = projection.snapshot({"bob", true}); CHECK(observer.values.size() == 1 && observer.values.contains("hp"));
        CHECK(call("invalid").status == fsnet::Status::internal);
        CHECK(call("overflow").status == fsnet::Status::internal);
        CHECK(call("fail").status == fsnet::Status::internal); CHECK(events.size() == 2);
        storage->fail = true;
        CHECK(call("damage", "50").status == fsnet::Status::rejected); CHECK(events.size() == 2);
        storage->fail = false;
        who = {fsnet::Caller::client, "alice"}; CHECK(call("read").data == "80:2");
        auto handle = entities->handle("player", "42"); CHECK(handle);
        std::optional<fsnet::Result> released;
        entities->deactivate(*handle, [&](fsnet::Result r) { released = std::move(r); });
        for (int i=0; !released && i<1000; ++i) { entities->poll(); std::this_thread::sleep_for(1ms); }
        CHECK(released && released->status == fsnet::Status::ok && entities->stats().transient_bytes == 0);
        CHECK(call("read").data == "80:0"); // persistent fields recover, transient fields reset
        entities->on_attributes([](const auto&) { throw std::runtime_error("transport unavailable"); });
        who = {fsnet::Caller::server, "logic"}; CHECK(call("damage", "5").data == "75:1");
        CHECK(entities->stats().publication_failures == 1);
        engine.stop(); CHECK(engine.memory_used() == 0);
        entities->stop(); fs::remove_all(directory);
        std::cout << "PASS Lua attributes: automatic commit events, file-only storage, ACL filtering, rollback, transient lifecycle and listener failure isolation\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; fs::remove_all(directory); return 1; }
}

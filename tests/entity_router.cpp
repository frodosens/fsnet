#include "fsnet/entity_router.hpp"
#include "fsnet/entity_host.hpp"
#include "fsnet/entity_directory.hpp"
#include "fsnet/entity_message.hpp"
#include <chrono>
#include <iostream>
#include <thread>

using namespace std::chrono_literals;
#define CHECK(c) do { if (!(c)) throw std::runtime_error(std::string(__FILE__) + ":" + std::to_string(__LINE__) + " " #c); } while (false)
void bind(fsnet::EntityScriptHost& host) {
    host.bind_legacy_stateful("counter.append", "counter", "", [](std::string input, fsnet::Reply reply) {
        auto entity = fsnet::parse_entity_message(input); CHECK(entity);
        auto state = fsnet::parse_state_message(entity->payload); CHECK(state);
        auto next = state->state + state->payload;
        reply({fsnet::Status::ok, fsnet::state_message(next, next)});
    });
}
template<class Pump, class Predicate> void until(Pump pump, Predicate done) {
    auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!done()) { pump(); CHECK(std::chrono::steady_clock::now() < deadline); std::this_thread::sleep_for(1ms); }
}
void metadata_views() {
    auto db = fsnet::memory_database();
    fsnet::EntityRecord record;
    record.owner = "a/boot-1"; record.expires_ms = 123456; record.fence = 17;
    record.snapshot.state.assign(2 * 1024 * 1024, 's');
    record.snapshot.receipts.push_back({"request", "Actor.method", std::string(65536, 'q'), std::string(65536, 'r')});
    for (bool delivery : {false, true}) {
        record.snapshot.delivery.epoch = delivery ? 99 : 0;
        for (unsigned version = 1; version <= 3; ++version) {
            record.initialized = version != 2;
            record.placement = version == 3 ? "b" : "";
            record.transfer_id = version == 3 ? 41 : 0;
            auto bytes = fsnet::encode_entity_record(record);
            auto view = fsnet::decode_entity_record_view(bytes);
            CHECK(view.owner == record.owner && view.fence == record.fence && view.expires_ms == record.expires_ms);
            CHECK(view.initialized == record.initialized && view.placement == record.placement && view.transfer_id == record.transfer_id);
            // The view refers directly to the source, including the multi-MiB
            // snapshot. Only the final location's small strings need copying.
            CHECK(view.owner.data() >= bytes.data() && view.owner.data() + view.owner.size() <= bytes.data() + bytes.size());
            CHECK(view.snapshot.data() >= bytes.data() && view.snapshot.data() + view.snapshot.size() == bytes.data() + bytes.size());
            auto full = fsnet::decode_entity_record(bytes);
            CHECK(full.snapshot.state == record.snapshot.state && full.snapshot.receipts.at(0).response == record.snapshot.receipts.at(0).response);
            auto key = fsnet::entity_record_key("Actor", "large");
            auto previous = db->get(key);
            CHECK(db->compare_exchange(key, previous ? previous->version : 0, bytes));
            auto location = fsnet::entity_location(*db, "Actor", "large");
            CHECK(location && location->owner == record.owner && location->placement == record.placement && location->transfer_id == record.transfer_id);
            auto rejected = [](std::string_view value) {
                try { fsnet::decode_entity_record_view(value); }
                catch (const std::exception&) { return true; }
                return false;
            };
            for (std::size_t cut = 0; cut < 96; ++cut) CHECK(rejected(std::string_view(bytes).substr(0, cut)));
            CHECK(rejected(std::string_view(bytes).substr(0, bytes.size() - 1)));
            CHECK(rejected(bytes + "trailing"));
            auto invalid = bytes; invalid[0] = 'X'; CHECK(rejected(invalid));
            auto metadata_end = std::size_t(4 + 4 + record.owner.size() + 8 + 8);
            invalid = bytes;
            invalid[metadata_end - 8] = static_cast<char>(0xff); CHECK(rejected(invalid)); // fence range
            if (version == 3) {
                invalid = bytes; invalid[metadata_end] = 2; CHECK(rejected(invalid));
                invalid = bytes; invalid[metadata_end + 1 + 4] = '/'; CHECK(rejected(invalid));
            }
            invalid = bytes; invalid[static_cast<std::size_t>(view.snapshot.data() - bytes.data())] = 'X'; CHECK(rejected(invalid));
        }
    }
    CHECK(!fsnet::entity_location(*db, "Actor", "missing"));
    std::cout << "PASS entity metadata views: FER1/2/3, FSP1/2, large payload and malformed envelopes\n";
}
void failover() {
    auto db = fsnet::memory_database(); fsnet::Node a("token"), b("token"), client("token");
    auto pa = std::make_shared<fsnet::NodeScriptHost>(a), pb = std::make_shared<fsnet::NodeScriptHost>(b);
    fsnet::EntityRuntimeLimits limits; limits.lease = 200ms; limits.request_timeout = 100ms;
    fsnet::EntityScriptHost ha(pa, db, "a/boot-1", limits), hb(pb, db, "b/boot-1", limits);
    bind(ha); bind(hb);
    fsnet::EntityRouter router(client, db, {{"a", "127.0.0.1", a.listen("127.0.0.1", 0), {"counter"}},
                                         {"b", "127.0.0.1", b.listen("127.0.0.1", 0), {"counter"}}});
    auto pump = [&] { a.poll(); b.poll(); client.poll(); ha.poll(); hb.poll(); router.poll(); };
    auto call = [&](const char* data, const char* request) {
        std::optional<fsnet::Result> result;
        router.legacy_call("counter", "42", "append", data, 2s, [&](fsnet::Result value) { result = std::move(value); }, request);
        until(pump, [&] { return result.has_value(); }); return *result;
    };
    CHECK(call("a", "one").data == "a"); CHECK(call("a", "one").data == "a");
    auto location = fsnet::entity_location(*db, "counter", "42"); CHECK(location);
    if (location->owner.starts_with("a/")) a.stop(); else b.stop();
    auto result = call("b", "two"); CHECK(result.status == fsnet::Status::ok && result.data == "ab");
    auto recovered = fsnet::entity_location(*db, "counter", "42"); CHECK(recovered && recovered->owner != location->owner);
    CHECK(call("b", "two").data == "ab");
    CHECK(router.pending() == 0);
    std::optional<fsnet::Result> cancelled;
    router.legacy_call("counter", "new", "append", "c", 1s, [&](fsnet::Result result) { cancelled = std::move(result); }, "cancel");
    router.stop(); CHECK(cancelled && cancelled->status == fsnet::Status::disconnected);
    std::cout << "PASS entity routing: ownership lookup, token replay, dead-node backoff, lease takeover and cancellation\n";
}
class DroppingHost final : public fsnet::ScriptHost {
    fsnet::NodeScriptHost parent;
public:
    bool drop = true;
    explicit DroppingHost(fsnet::Node& node) : parent(node) {}
    bool has_route(const std::string& route) const override { return parent.has_route(route); }
    void bind(std::string route, fsnet::Handler handler) override {
        parent.bind(std::move(route), [this, handler](std::string data, fsnet::Reply reply) {
            handler(std::move(data), [this, reply](fsnet::Result result) {
                if (drop) { drop = false; return; } // commit succeeds but its response is lost
                reply(std::move(result));
            });
        });
    }
    void unbind(const std::string& route) override { parent.unbind(route); }
    std::uint64_t connect(const std::string& address, std::uint16_t port) override { return parent.connect(address, port); }
    void call(std::uint64_t peer, std::string route, std::string data, std::chrono::milliseconds timeout, fsnet::Reply reply) override {
        parent.call(peer, std::move(route), std::move(data), timeout, std::move(reply));
    }
};
void lost_reply() {
    auto db = fsnet::memory_database(); fsnet::Node server("token"), client("token");
    auto parent = std::make_shared<DroppingHost>(server);
    fsnet::EntityScriptHost host(parent, db, "server/boot-1"); bind(host);
    fsnet::EntityRouter router(client, db, {{"server", "127.0.0.1", server.listen("127.0.0.1", 0), {"counter"}}});
    std::optional<fsnet::Result> result;
    router.legacy_call("counter", "42", "append", "once", 3s, [&](fsnet::Result value) { result = std::move(value); }, "request-1");
    until([&] { server.poll(); client.poll(); host.poll(); router.poll(); }, [&] { return result.has_value(); });
    CHECK(result->status == fsnet::Status::ok && result->data == "once" && host.stats().replayed == 1);
    std::cout << "PASS entity retry: lost acknowledgement replays committed response without repeating the state transition\n";
}
int main() {
    try { metadata_views(); failover(); lost_reply(); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}

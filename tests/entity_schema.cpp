#include "fsnet/entity_schema.hpp"
#include "fsnet/attribute_wire.hpp"
#include <limits>
#include <iostream>
#include <stdexcept>
#define CHECK(x) do { if (!(x)) throw std::runtime_error(#x); } while (false)
template<class F> void denied(F f) { bool caught = false; try { f(); } catch (const std::exception&) { caught = true; } CHECK(caught); }
int main() {
    using namespace fsnet;
    try {
        EntitySchema schema;
        schema.attributes.emplace("hp", AttributeDefinition{AttributeType::integer, std::int64_t(100), Access::read_write, Access::read, Visibility::observers, true});
        schema.attributes.emplace("gold", AttributeDefinition{AttributeType::integer, std::int64_t(10)});
        schema.attributes.emplace("secret", AttributeDefinition{AttributeType::string, std::string("hidden"), Access::read_write, Access::none, Visibility::none, true});
        schema.attributes.emplace("input", AttributeDefinition{AttributeType::string, std::string{}, Access::read_write, Access::read_write, Visibility::owner, false});
        schema.methods.emplace("move", RpcPolicy{true, false, true});
        schema.methods.emplace("damage", RpcPolicy{false, true, false});
        EntityAttributes entity(schema, "alice");
        CallIdentity server{Caller::server, "logic"}, alice{Caller::client, "alice"}, bob{Caller::client, "bob"};
        CHECK(schema.methods.at("move").allows(alice, "alice"));
        CHECK(!schema.methods.at("move").allows(bob, "alice"));
        CHECK(!schema.methods.at("damage").allows(alice, "alice"));
        CHECK(schema.methods.at("damage").allows(server, "alice"));
        denied([&] { entity.stage({{"hp", std::int64_t(999)}}, alice); });
        denied([&] { entity.stage({{"input", std::string("spoof")}}, bob); });
        denied([&] { entity.stage({{"hp", std::string("wrong")}}, server); });
        denied([&] { entity.read("secret", alice); });
        denied([&] { entity.read("gold", bob); });
        auto before = entity.stage({{"hp", std::int64_t(80)}, {"gold", std::int64_t(20)}}, server);
        CHECK(entity.revision() == 0); // staging / failed storage commits publish nothing
        CHECK(std::get<std::int64_t>(entity.read("hp", server)) == 100);
        CHECK(entity.publish(before)); CHECK(entity.revision() == 1);
        CHECK(!entity.publish(before)); // stale commit cannot publish
        auto owner = entity.changes(0, {"alice", true});
        auto observer = entity.changes(0, {"bob", true});
        CHECK(owner.values.size() == 2 && observer.values.size() == 1 && observer.values.contains("hp"));
        CHECK(entity.snapshot({"bob", false}).values.empty());
        CHECK(!entity.snapshot({"alice", true}).values.contains("secret"));
        CHECK(entity.changes(1, {"alice", true}).values.empty());
        CHECK(entity.changes(100, {"alice", true}).snapshot);
        auto input = entity.stage({{"input", std::string("forward")}}, alice); CHECK(entity.publish(input));
        CHECK(!entity.persistent().contains("input"));
        EntityAttributes restored(schema, "alice"); restored.restore(entity.persistent(), entity.revision());
        CHECK(std::get<std::int64_t>(restored.read("hp", server)) == 80);
        CHECK(std::get<std::string>(restored.read("input", server)).empty());
        auto invalid = entity.stage({}, server); invalid.values = {{"gold", std::int64_t(99)}, {"hp", std::string("bad")}};
        denied([&] { entity.publish(invalid); });
        CHECK(std::get<std::int64_t>(entity.read("gold", server)) == 20);
        AttributeUpdate full{0, 7, true, {{"negative", std::numeric_limits<std::int64_t>::min()}, {"binary", std::string("a\0b", 3)}, {"real", 0.25}, {"flag", true}}};
        auto bytes = encode_attributes(full);
        CHECK(decode_attributes(bytes).values == full.values);
        for (std::size_t i = 0; i < bytes.size(); ++i) denied([&] { decode_attributes(std::string_view(bytes).substr(0, i)); });
        denied([&] { decode_attributes(bytes + "trailing"); });
        auto duplicate = encode_attributes({0, 1, true, {{"x", true}}}); duplicate[22] = 2;
        duplicate += duplicate.substr(23); denied([&] { decode_attributes(duplicate); });
        denied([&] { encode_attributes({0, 1, true, {{"nan", std::numeric_limits<double>::quiet_NaN()}}}); });
        AttributeReplica replica;
        CHECK(!replica.apply(AttributeUpdate{7, 8, false, {{"flag", false}}}));
        CHECK(replica.apply(bytes));
        CHECK(!replica.apply(AttributeUpdate{6, 8, false, {{"flag", false}}})); CHECK(replica.revision() == 7);
        CHECK(replica.apply(AttributeUpdate{7, 9, false, {{"flag", false}}}));
        CHECK(!std::get<bool>(replica.values().at("flag")));
        CHECK(!replica.apply(full)); // late old snapshot cannot roll back
        CHECK(replica.apply(AttributeUpdate{0, 9, true, {{"flag", false}}})); CHECK(replica.values().size() == 1);
        schema.methods.emplace("unrestricted", RpcPolicy{}); denied([&] { schema.validate(); });
        std::cout << "PASS schema permissions, filtered snapshots/deltas, staged publication, persistence and stale revision rejection\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}

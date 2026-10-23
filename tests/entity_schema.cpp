#include "fsnet/entity_schema.hpp"
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
        schema.methods.emplace("unrestricted", RpcPolicy{}); denied([&] { schema.validate(); });
        std::cout << "PASS schema permissions, filtered snapshots/deltas, staged publication, persistence and stale revision rejection\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}

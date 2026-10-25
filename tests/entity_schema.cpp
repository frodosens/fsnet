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
        auto alice_connection=ConnectionRef::create(),bob_connection=ConnectionRef::create();
        auto owner_key=alice_connection.key();
        EntitySchema schema;
        schema.attributes.emplace("hp", AttributeDefinition{AttributeType::integer, std::int64_t(100), Access::read_write, Access::read, Visibility::observers, true});
        schema.attributes.emplace("gold", AttributeDefinition{AttributeType::integer, std::int64_t(10)});
        schema.attributes.emplace("secret", AttributeDefinition{AttributeType::string, std::string("hidden"), Access::read_write, Access::none, Visibility::none, true});
        schema.attributes.emplace("input", AttributeDefinition{AttributeType::string, std::string{}, Access::read_write, Access::read_write, Visibility::owner, false});
        schema.methods.emplace("move", RpcPolicy{true, false, true});
        schema.methods.emplace("damage", RpcPolicy{false, true, false});
        EntityAttributes entity(schema, owner_key);
        CallIdentity server{Caller::server, "logic"}, alice{Caller::client, "same",0,alice_connection}, bob{Caller::client, "same",0,bob_connection};
        CHECK(schema.methods.at("move").allows(alice, owner_key));
        CHECK(!schema.methods.at("move").allows(bob, owner_key));
        CHECK(!schema.methods.at("damage").allows(alice, owner_key));
        CHECK(schema.methods.at("damage").allows(server, owner_key));
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
        auto owner = entity.changes(0, {true,alice_connection});
        auto observer = entity.changes(0, {true,bob_connection});
        CHECK(owner.values.size() == 2 && observer.values.size() == 1 && observer.values.contains("hp"));
        CHECK(entity.snapshot({false,bob_connection}).values.empty());
        CHECK(!entity.snapshot({true,alice_connection}).values.contains("secret"));
        CHECK(entity.changes(1, {true,alice_connection}).values.empty());
        CHECK(entity.changes(100, {true,alice_connection}).snapshot);
        auto input = entity.stage({{"input", std::string("forward")}}, alice); CHECK(entity.publish(input));
        CHECK(!entity.persistent().contains("input"));
        EntityAttributes restored(schema, owner_key); restored.restore(entity.persistent(), entity.revision());
        CHECK(std::get<std::int64_t>(restored.read("hp", server)) == 80);
        CHECK(std::get<std::string>(restored.read("input", server)).empty());
        auto invalid = entity.stage({}, server); invalid.values = {{"gold", std::int64_t(99)}, {"hp", std::string("bad")}};
        denied([&] { entity.publish(invalid); });
        CHECK(std::get<std::int64_t>(entity.read("gold", server)) == 20);
        AttributeUpdate full{0, 7, true, {{"negative", std::numeric_limits<std::int64_t>::min()}, {"binary", std::string("a\0b", 3)}, {"real", 0.25}, {"flag", true}}};
        auto bytes = encode_attributes(full);
        CHECK(decode_attributes(bytes).values == full.values);
        CHECK(attribute_wire_size(full)==bytes.size());
        CHECK(attribute_wire_size(full.values)==bytes.size());
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
        // Sparse updates must enforce limits on the merged image, and leave
        // every field/revision untouched if a later field exceeds a limit.
        AttributeReplica bounded;
        AttributeUpdate many{0,1,true,{}};
        for(int i=0;i<256;++i) many.values.emplace("p"+std::to_string(i),false);
        CHECK(bounded.apply(many)); auto saved=bounded.values();
        denied([&] { bounded.apply({1,2,false,{{"p0",true},{"z_new",true}}}); });
        CHECK(bounded.values()==saved && bounded.revision()==1);
        AttributeUpdate large{0,1,true,{}};
        for(int i=0;i<15;++i) large.values.emplace("p"+std::to_string(i),std::string(65536,'x'));
        CHECK(bounded.apply(large)); saved=bounded.values();
        denied([&] { bounded.apply({1,2,false,{{"p0",true},{"q",std::string(65536,'x')},{"r",std::string(65536,'x')}}}); });
        CHECK(bounded.values()==saved && bounded.revision()==1);
        AttributeArray nodes(256,AttributeValue(false));
        AttributeUpdate wide{0,2,true,{}};
        for(int i=0;i<15;++i) wide.values.emplace("p"+std::to_string(i),nodes);
        CHECK(bounded.apply(wide)); saved=bounded.values();
        denied([&] { bounded.apply({2,3,false,{{"p0",true},{"q",nodes},{"r",nodes}}}); });
        CHECK(bounded.values()==saved && bounded.revision()==2);
        CHECK(bounded.apply({2,3,false,{{"p0",true},{"q",false}}}));
        CHECK(bounded.values().size()==16 && bounded.revision()==3);
        auto revision=entity.revision();
        CHECK(entity.publish({revision,{{"hp",std::int64_t(80)}}}));
        CHECK(entity.revision()==revision && entity.changes(revision,{true,alice_connection}).values.empty());
        schema.methods.emplace("unrestricted", RpcPolicy{}); denied([&] { schema.validate(); });
        std::cout << "PASS schema permissions, filtered snapshots/deltas, staged publication, persistence and stale revision rejection\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}

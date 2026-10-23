#include "fsnet/attribute_wire.hpp"
#include <iostream>
#include <random>
#define CHECK(x) do { if(!(x)) throw std::runtime_error(#x); } while(false)
template<class F> void denied(F f) { bool caught=false; try { f(); } catch(const std::exception&) { caught=true; } CHECK(caught); }
int main() {
    using namespace fsnet;
    try {
        AttributeArray items; AttributeMap tasks;
        for(int i=0;i<128;++i) { items.emplace_back(AttributeStruct{{"id",std::to_string(i)},{"count",std::int64_t(1)}}); tasks.emplace(std::to_string(i),items.back()); }
        AttributeUpdate previous{0,1,true,{{"items",items},{"tasks",tasks},{"scalar",false}}};
        auto next=previous; next.revision=2;
        std::get<AttributeStruct>(std::get<AttributeArray>(next.values.at("items"))[64]).at("count")=std::int64_t(2);
        std::get<AttributeMap>(next.values.at("tasks")).erase("7");
        std::get<AttributeMap>(next.values.at("tasks"))["new"]=AttributeStruct{{"id",std::string("new")},{"count",std::int64_t(2)}};
        next.values.at("scalar")=true;
        auto delta=attribute_delta(previous,next); CHECK(delta.patches.size()==3 && delta.values.size()==1);
        auto bytes=encode_attributes(delta); CHECK(bytes.starts_with("FSA2") && bytes.size()==attribute_wire_size(delta));
        auto decoded=decode_attributes(bytes); CHECK(encode_attributes(decoded)==bytes);
        AttributeReplica replica; CHECK(replica.apply(previous)); CHECK(replica.apply(decoded)); CHECK(replica.values()==next.values);
        CHECK(!replica.apply(decoded)); CHECK(replica.values()==next.values);
        for(std::size_t n=0;n<bytes.size();++n) denied([&]{decode_attributes(std::string_view(bytes).substr(0,n));});
        denied([&]{decode_attributes(bytes+"trailing");});
        auto bad=delta; bad.snapshot=true; bad.base=0; denied([&]{encode_attributes(bad);});
        bad=delta; bad.values["items"]=items; denied([&]{encode_attributes(bad);});
        // An early valid edit followed by an invalid edit must not publish anything.
        bad=delta; bad.base=2; bad.revision=3; bad.patches.back().map=false; bad.patches.back().key.clear(); bad.patches.back().value.reset(); bad.patches.back().index=256;
        denied([&]{replica.apply(bad);}); CHECK(replica.values()==next.values && replica.revision()==2);
        bad={2,3,false,{}}; AttributePatch missing; missing.property="tasks"; missing.map=true; missing.key="absent"; bad.patches.push_back(missing);
        denied([&]{replica.apply(bad);}); CHECK(replica.values()==next.values);
        // Combined replacement + edits cannot evade the aggregate image limit.
        bad={2,3,false,{{"scalar",AttributeArray(256,AttributeArray(14,false))}}};
        AttributePatch edit; edit.property="items"; edit.index=0; edit.remove=0; edit.insert={false}; bad.patches.push_back(edit);
        denied([&]{replica.apply(bad);}); CHECK(replica.values()==next.values && replica.revision()==2);
        // Random ordered insert/erase/update sequences exercise splice indices,
        // map removals and growth/shrink. Seed is fixed for reproducibility.
        std::mt19937 rng(1257); auto current=next;
        for(int round=0;round<600;++round) {
            auto changed=current; ++changed.revision;
            auto& list=std::get<AttributeArray>(changed.values.at("items"));
            auto& map=std::get<AttributeMap>(changed.values.at("tasks"));
            auto action=rng()%3;
            if(action==0 && !list.empty()) list.erase(list.begin()+rng()%list.size());
            else if(list.size()<200) list.insert(list.begin()+rng()%(list.size()+1),std::int64_t(round));
            if(!list.empty() && round%2) list[rng()%list.size()]=std::int64_t(round+1);
            auto key=std::to_string(rng()%128);
            if(action==0) map.erase(key); else map[key]=std::int64_t(round);
            auto update=attribute_delta(current,changed); auto encoded=encode_attributes(update);
            CHECK(replica.apply(decode_attributes(encoded))); CHECK(replica.values()==changed.values);
            current=std::move(changed);
        }
        auto empty=current; empty.values.at("items")=AttributeArray{}; empty.values.at("tasks")=AttributeMap{}; ++empty.revision;
        CHECK(replica.apply(attribute_delta(current,empty))); CHECK(replica.values()==empty.values);
        std::cout<<"PASS adaptive patches: sparse edits, fallbacks, codec, bounds, atomic failure and randomized replay\n";
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n'; return 1;}
}

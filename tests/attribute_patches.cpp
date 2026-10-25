#include "fsnet/attribute_wire.hpp"
#include "fsnet/replication_wire.hpp"
#include <iostream>
#include <random>
#define CHECK(x) do { if(!(x)) throw std::runtime_error(#x); } while(false)
template<class F> void denied(F f) { bool caught=false; try { f(); } catch(const std::exception&) { caught=true; } CHECK(caught); }
int main() {
    using namespace fsnet;
    try {
        // A 1 KiB payload must stay out of a one-field delta.
        AttributeStruct data{{"payload",std::string(1024,'p')},{"position",AttributeStruct{{"x",0.0},{"y",0.0},{"z",0.0}}}};
        AttributeUpdate large{0,1,true,{{"data",data}}}, changed=large; changed.revision=2;
        std::get<AttributeStruct>(std::get<AttributeStruct>(changed.values.at("data")).at("position")).at("x")=10.0;
        auto field=attribute_delta(large,changed); CHECK(field.values.empty() && field.patches.size()==1);
        CHECK(field.patches[0].replace && field.patches[0].path.size()==2);
        CHECK(std::get<std::string>(field.patches[0].path[0])=="position" && std::get<std::string>(field.patches[0].path[1])=="x");
        auto field_bytes=encode_attributes(field); CHECK(field_bytes.starts_with("FSA3") && field_bytes.size()<100);
        CHECK(field_bytes.find(std::string(64,'p'))==std::string::npos);
        CHECK(field_bytes.size()==attribute_wire_size(field));
        // Retained versions/copies stay immutable through edits, errors and reset.
        AttributeReplica versioned; CHECK(!versioned.snapshot()); CHECK(versioned.apply(large));
        auto retained=versioned.snapshot(); CHECK(retained==versioned.snapshot());
        auto charge=attribute_wire_size(large)+attribute_image_bytes(large.values);
        CHECK(versioned.image_bytes()==charge);
        bool limited=false;try{versioned.apply(field,1);}catch(const AttributeReplicaBudget&){limited=true;}
        CHECK(limited&&versioned.snapshot()==retained&&versioned.image_bytes()==charge);
        auto independent=versioned; CHECK(versioned.apply(field));
        CHECK(retained->values==large.values && retained->revision==1);
        CHECK(independent.values()==large.values && versioned.values()==changed.values);
        auto moved=std::move(independent); CHECK(!independent.snapshot() && !independent.initialized());
        CHECK(moved.values()==large.values); independent=std::move(moved);
        CHECK(!moved.apply({1,2,false,{{"counter",std::int64_t(1)}}}));
        auto current_version=versioned.snapshot();
        auto failed=field;failed.base=2;failed.revision=3;failed.patches.front().path[0]=std::string("absent");
        denied([&]{versioned.apply(failed);}); CHECK(versioned.snapshot()==current_version);
        CHECK(!versioned.apply({999,1000,false,{}})); CHECK(versioned.snapshot()==current_version);
        versioned.reset(); CHECK(!versioned.snapshot() && current_version->values==changed.values);
        AttributeUpdate splice{2,3,false,{}};
        AttributePatch wire_splice;wire_splice.property="items";wire_splice.insert={std::int64_t(3)};splice.patches.push_back(wire_splice);
        for(const auto& update:{large,field,splice}) {
            auto wire=encode_replication_packet(7,"Actor","alice",Status::ok,&update,30000);
            auto view=decode_replication_packet_view(wire);auto owned=decode_replication_packet(wire);
            CHECK(view.type==owned.type && view.id==owned.id && view.subscription==7 && view.update->revision==update.revision);
            for(std::size_t n=0;n<wire.size();++n)denied([&]{decode_replication_packet_view(std::string_view(wire).substr(0,n));});
            auto original=wire;remap_replication_subscription(wire,999);
            CHECK(decode_replication_packet(wire).subscription==999 && wire.substr(13)==original.substr(13) && wire.substr(0,5)==original.substr(0,5));
            auto presence=std::string{char(1),char(3)}+original;
            remap_replication_subscription(presence,1000,2);CHECK(presence.substr(15)==original.substr(13));
            CHECK(decode_replication_packet_view(std::string_view(presence).substr(2)).subscription==1000);
            auto corrupt=original;corrupt.back()=char(255);corrupt+='x';denied([&]{decode_replication_packet_view(corrupt);});
        }
        auto revoked=encode_replication_packet(7,"Actor","alice",Status::rejected);
        CHECK(!decode_replication_packet_view(revoked).update);
        denied([&]{remap_replication_subscription(revoked,0);});
        denied([&]{decode_replication_packet_view(revoked+"x");});
        AttributeReplica fields; CHECK(fields.apply(large)); CHECK(fields.apply(field_bytes)); CHECK(fields.values()==changed.values);
        std::cout<<"nested_struct_1kb full="<<encode_attributes({1,2,false,changed.values}).size()<<" field="<<field_bytes.size()<<'\n';
        for(std::size_t n=0;n<field_bytes.size();++n) denied([&]{decode_attributes(std::string_view(field_bytes).substr(0,n));});
        auto invalid=field; invalid.base=2; invalid.revision=3;
        invalid.patches.push_back(invalid.patches.front()); invalid.patches.back().path[1]=std::string("missing");
        denied([&]{fields.apply(invalid);}); CHECK(fields.values()==changed.values && fields.revision()==2);
        invalid=field; invalid.base=2; invalid.revision=3; invalid.patches[0].path[0]=std::size_t(0);
        denied([&]{fields.apply(invalid);}); CHECK(fields.values()==changed.values && fields.revision()==2);
        invalid=field; invalid.patches[0].path.assign(9,std::string("child")); denied([&]{encode_attributes(invalid);});
        invalid=field; invalid.patches[0].path[0]=std::size_t(256); denied([&]{encode_attributes(invalid);});
        invalid=field; invalid.patches[0].value.reset(); denied([&]{encode_attributes(invalid);});
        // Uplink and downlink use the same generator and patch applicator.
        auto proposal=attribute_write_delta(large.values,1,changed.values,1);
        auto same_field=field; same_field.revision=1;
        CHECK(encode_attributes(proposal)==encode_attributes(same_field));
        AttributeDefinition number{AttributeType::number,0.0};
        AttributeDefinition position{AttributeType::structure,std::get<AttributeStruct>(data.at("position"))};
        position.fields={{"x",number},{"y",number},{"z",number}};
        AttributeDefinition definition{AttributeType::structure,data};
        definition.fields={{"payload",{AttributeType::string,std::string(1024,'p')}},{"position",position}};
        definition.client=Access::read_write;
        EntitySchema schema; schema.attributes.emplace("data",definition);
        definition.client=Access::read; schema.attributes.emplace("locked",definition);
        EntityAttributes authority(schema,"alice"); authority.restore_full(large.values,1);
        CallIdentity alice{Caller::client,"alice"},bob{Caller::client,"bob"};
        auto staged=stage_attribute_proposal(authority,decode_attributes(encode_attributes(proposal)),alice);
        CHECK(staged.values.at("data")==changed.values.at("data"));
        CHECK(authority.all_values().at("data")==large.values.at("data") && authority.revision()==1);
        denied([&]{stage_attribute_proposal(authority,proposal,bob);});
        auto forbidden=proposal; forbidden.patches[0].property="locked";
        denied([&]{stage_attribute_proposal(authority,forbidden,alice);});
        auto mistyped=proposal; mistyped.patches[0].value=std::string("bad");
        denied([&]{stage_attribute_proposal(authority,mistyped,alice);});
        auto unknown=proposal; unknown.patches[0].path.back()=std::string("missing");
        denied([&]{stage_attribute_proposal(authority,unknown,alice);});
        auto stale=proposal; stale.base=stale.revision=0;
        denied([&]{stage_attribute_proposal(authority,stale,alice);});
        auto readonly_noop=attribute_write_delta(authority.all_values(),1,{{"locked",data}},1);
        CHECK(readonly_noop.values.contains("locked"));
        denied([&]{stage_attribute_proposal(authority,readonly_noop,alice);});
        auto explicit_noop=attribute_write_delta(large.values,1,large.values,1);
        CHECK(explicit_noop.values.contains("data"));
        CHECK(stage_attribute_proposal(authority,explicit_noop,alice).values.contains("data"));
        CHECK(attribute_write_delta(large.values,2,changed.values,1).patches.empty());
        CHECK(authority.all_values().at("data")==large.values.at("data") && authority.revision()==1);
        CHECK(authority.publish(staged)); CHECK(authority.revision()==2);
        // Invalid depth/component tags/indices are rejected while decoding too.
        auto malformed=field_bytes; auto path_offset=25+1+std::string("data").size();
        malformed[path_offset]=9; denied([&]{decode_attributes(malformed);});
        malformed=field_bytes; malformed[path_offset+1]=2; denied([&]{decode_attributes(malformed);});
        malformed=field_bytes; malformed[path_offset+1]=0; malformed[path_offset+2]=1; malformed[path_offset+3]=0;
        denied([&]{decode_attributes(malformed);});
        // A map key containing a dot is one component; sequence indices are
        // numeric components. Keep stable values outside the edited leaf.
        AttributeUpdate mixed{0,1,true,{{"tasks",AttributeMap{{"daily.1",AttributeArray{data}}}}}};
        auto mixed_next=mixed; mixed_next.revision=2;
        auto& entry=std::get<AttributeArray>(std::get<AttributeMap>(mixed_next.values.at("tasks")).at("daily.1"))[0];
        std::get<AttributeStruct>(std::get<AttributeStruct>(entry).at("position")).at("z")=7.0;
        auto nested=attribute_delta(mixed,mixed_next); CHECK(nested.patches.size()==1 && nested.patches[0].path.size()==4);
        CHECK(std::get<std::string>(nested.patches[0].path[0])=="daily.1" && std::get<std::size_t>(nested.patches[0].path[1])==0);
        AttributeReplica mixed_replica; CHECK(mixed_replica.apply(mixed)); CHECK(mixed_replica.apply(encode_attributes(nested))); CHECK(mixed_replica.values()==mixed_next.values);
        // Maximum depth remains eight, including containers inside replacement values.
        AttributeValue deep_before=0.0,deep_after=1.0;
        for(int i=0;i<8;++i) {deep_before=AttributeStruct{{"child",deep_before},{"payload",std::string(64,'p')}}; deep_after=AttributeStruct{{"child",deep_after},{"payload",std::string(64,'p')}};}
        // The payload sibling at the last level is within the same depth budget.
        AttributeUpdate deepest{0,1,true,{{"deep",deep_before}}},deepest_next{0,2,true,{{"deep",deep_after}}};
        auto deep_delta=attribute_delta(deepest,deepest_next); CHECK(deep_delta.patches.size()==1 && deep_delta.patches[0].path.size()==8);
        AttributeReplica deep_replica; CHECK(deep_replica.apply(deepest)); CHECK(deep_replica.apply(encode_attributes(deep_delta))); CHECK(deep_replica.values()==deepest_next.values);
        invalid=deep_delta; invalid.patches[0].value=AttributeArray{0.0}; denied([&]{encode_attributes(invalid);});
        // Dense/small structures may cost less as one replacement.
        AttributeUpdate tiny{0,1,true,{{"v",AttributeStruct{{"x",0.0},{"y",0.0},{"z",0.0}}}}},tiny_next=tiny;
        tiny_next.revision=2; tiny_next.values.at("v")=AttributeStruct{{"x",1.0},{"y",2.0},{"z",3.0}};
        CHECK(attribute_delta(tiny,tiny_next).patches.empty());
        tiny_next=tiny; tiny_next.revision=2;
        std::get<AttributeStruct>(tiny_next.values.at("v")).at("x")=1.0;
        auto one_axis=attribute_delta(tiny,tiny_next);
        CHECK(one_axis.patches.size()==1 && one_axis.patches[0].replace && one_axis.patches[0].path.size()==1);
        // Old FSA2 append/splice remains readable and encodes identically.
        AttributePatch append; append.property="list"; append.index=0; append.insert={true};
        AttributeUpdate legacy{1,2,false,{},{append}}; auto legacy_bytes=encode_attributes(legacy);
        CHECK(legacy_bytes.starts_with("FSA2") && encode_attributes(decode_attributes(legacy_bytes))==legacy_bytes);
        AttributeArray items; AttributeMap tasks;
        for(int i=0;i<128;++i) { items.emplace_back(AttributeStruct{{"id",std::to_string(i)},{"count",std::int64_t(1)}}); tasks.emplace(std::to_string(i),items.back()); }
        AttributeUpdate previous{0,1,true,{{"items",items},{"tasks",tasks},{"scalar",false}}};
        auto next=previous; next.revision=2;
        std::get<AttributeStruct>(std::get<AttributeArray>(next.values.at("items"))[64]).at("count")=std::int64_t(2);
        std::get<AttributeMap>(next.values.at("tasks")).erase("7");
        std::get<AttributeMap>(next.values.at("tasks"))["new"]=AttributeStruct{{"id",std::string("new")},{"count",std::int64_t(2)}};
        next.values.at("scalar")=true;
        auto delta=attribute_delta(previous,next); CHECK(delta.patches.size()==3 && delta.values.size()==1);
        auto bytes=encode_attributes(delta); CHECK(bytes.starts_with("FSA3") && bytes.size()==attribute_wire_size(delta));
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
            CHECK(replica.image_bytes()==attribute_wire_size(changed.values)+attribute_image_bytes(changed.values));
            current=std::move(changed);
        }
        auto empty=current; empty.values.at("items")=AttributeArray{}; empty.values.at("tasks")=AttributeMap{}; ++empty.revision;
        CHECK(replica.apply(attribute_delta(current,empty))); CHECK(replica.values()==empty.values);
        std::cout<<"PASS adaptive patches: sparse edits, fallbacks, codec, bounds, atomic failure and randomized replay\n";
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n'; return 1;}
}

#include "fsnet/lua_engine.hpp"
#include "fsnet/entity_host.hpp"
#include "fsnet/client_script_host.hpp"
#include <chrono>
#include <iomanip>
#include <iostream>
#include <optional>
using Clock=std::chrono::steady_clock;
using namespace std::chrono_literals;
void require(bool ok,const char* message) { if(!ok) throw std::runtime_error(message); }
struct CountingHost final:fsnet::ScriptHost {
    std::shared_ptr<fsnet::ClientScriptHost> client;
    std::size_t writes=0,proposal_bytes=0;
    explicit CountingHost(std::shared_ptr<fsnet::ClientScriptHost> value):client(std::move(value)) {}
    bool has_route(const std::string& name) const override {return client->has_route(name);}
    void bind(std::string name,fsnet::Handler fn) override {client->bind(std::move(name),std::move(fn));}
    void unbind(const std::string& name) override {client->unbind(name);}
    std::uint64_t connect(const std::string& address,std::uint16_t port) override {return client->connect(address,port);}
    void call(std::uint64_t peer,std::string route,std::string payload,std::chrono::milliseconds timeout,fsnet::Reply reply) override {client->call(peer,std::move(route),std::move(payload),timeout,std::move(reply));}
    void call_entity(std::string route,std::string payload,std::chrono::milliseconds timeout,fsnet::Reply reply) override {client->call_entity(std::move(route),std::move(payload),timeout,std::move(reply));}
    void watch_attributes(std::string type,std::string id,AttributeReady ready,std::chrono::milliseconds timeout) override {client->watch_attributes(std::move(type),std::move(id),std::move(ready),timeout);}
    std::shared_ptr<const fsnet::AttributeUpdate> attribute_snapshot(std::uint64_t id) const override {return client->attribute_snapshot(id);}
    void unwatch_attributes(std::uint64_t id) override {client->unwatch_attributes(id);}
    void write_attributes(std::uint64_t id,fsnet::EntityAttributes::Values values,fsnet::Reply reply,fsnet::AttributeWriteOptions options,std::chrono::milliseconds timeout) override {
        ++writes; auto revision=options.expected_revision.value_or(0);
        proposal_bytes+=fsnet::attribute_wire_size(fsnet::AttributeUpdate{revision,revision,false,values});
        client->write_attributes(id,std::move(values),std::move(reply),std::move(options),timeout);
    }
};
std::string model() {return R"(
    local Item=fsnet.struct('BenchItem',{id='string',count='int32'})
    local initial={}; for i=1,128 do initial[i]={id='item-'..i,count=0} end
    local E=fsnet.Entity:extend('BatchBench',{owner='id'})
    E:attr('items',{type=fsnet.list(Item,256),default=initial,client='rw',sync='observers'})
)";}
void network(bool batched,std::size_t rounds) {
    fsnet::Node server("container-bench"),node("container-bench");
    server.identify([](std::uint64_t,const std::string&){return std::optional(fsnet::CallIdentity{fsnet::Caller::client,"alice"});});
    auto transport=std::make_shared<fsnet::NodeScriptHost>(server);
    auto host=std::make_shared<fsnet::EntityScriptHost>(transport,fsnet::memory_database(),"bench/boot");
    fsnet::AttributeReplication replication(server,host); fsnet::LuaEngine authority(host); authority.load(model()+"E:register()");
    fsnet::Reply gate; bool done=false;
    server.bind("ready",[&](std::string,fsnet::Reply reply){gate=std::move(reply);});
    server.bind("done",[&](std::string,fsnet::Reply reply){done=true; reply({fsnet::Status::ok,{}});});
    auto port=server.listen("127.0.0.1",0); auto peer=node.connect("127.0.0.1",port);
    auto client=std::make_shared<fsnet::ClientScriptHost>(node,peer); auto counted=std::make_shared<CountingHost>(client);
    fsnet::LuaEngine engine(counted);
    std::string edit="for i=1,16 do value.items[i].count=round end";
    std::string operation=batched?"entity:batch(function(value) "+edit+" end)":"local value=entity; "+edit;
    engine.load(model()+"E:define(); runtime.start(function() local entity=E:watch('alice'); runtime.server_request('ready',''); for round=1,"+std::to_string(rounds)+" do "+operation+" end; assert(entity.items[1].count=="+std::to_string(rounds)+" and entity.items[16].count=="+std::to_string(rounds)+"); runtime.server_request('done',''); entity:close() end)");
    auto pump=[&]{server.poll(); node.poll(); host->poll(); replication.poll(); client->poll(); authority.poll(); engine.poll();};
    auto until=[&](auto condition){auto deadline=Clock::now()+60s; while(!condition()){pump(); if(Clock::now()>deadline) throw std::runtime_error("benchmark deadline: "+engine.last_error()+" / "+authority.last_error());}};
    until([&]{return bool(gate);});
    auto commits=host->stats().committed; auto begin=Clock::now(); auto release=std::move(gate); release({fsnet::Status::ok,{}});
    until([&]{return done && !engine.active();}); auto elapsed=std::chrono::duration<double>(Clock::now()-begin).count();
    require(engine.last_error().empty(),"Lua benchmark failed");
    auto expected=rounds*(batched?1:16); require(counted->writes==expected,"write count mismatch");
    require(host->stats().committed-commits==expected,"commit count mismatch");
    std::cout<<std::setprecision(10)<<"{\"scenario\":\""<<(batched?"batch16":"sequential16")<<"\",\"rounds\":"<<rounds<<",\"us_per_round\":"<<elapsed*1e6/rounds
        <<",\"writes\":"<<counted->writes<<",\"commits\":"<<host->stats().committed-commits<<",\"proposal_fsa_bytes\":"<<counted->proposal_bytes<<"}\n";
    engine.stop(); require(engine.memory_used()==0,"client cleanup"); client->stop(); authority.stop(); replication.stop(); host->stop();
}
void wire() {
    using namespace fsnet;
    AttributeArray list; AttributeMap map;
    for(int i=0;i<128;++i) { auto value=AttributeStruct{{"id",std::string("item-")+std::to_string(i)},{"count",std::int64_t(1)}}; list.emplace_back(value); map.emplace(std::to_string(i),value); }
    {
        AttributeUpdate before{0,1,true,{{"data",AttributeStruct{{"payload",std::string(1024,'p')},{"position",AttributeStruct{{"x",0.0},{"y",0.0},{"z",0.0}}}}}}};
        auto after=before; after.revision=2;
        std::get<AttributeStruct>(std::get<AttributeStruct>(after.values.at("data")).at("position")).at("x")=1.0;
        auto full=encode_attributes({1,2,false,after.values}); auto update=attribute_delta(before,after); auto delta=encode_attributes(update);
        AttributeReplica replica; require(replica.apply(before) && replica.apply(delta) && replica.values()==after.values,"field equivalence");
        std::cout<<"{\"scenario\":\"nested_struct_1kb\",\"full_fsa_bytes\":"<<full.size()<<",\"patch_fsa_bytes\":"<<delta.size()<<",\"patches\":"<<update.patches.size()<<"}\n";
    }
    {
        AttributeUpdate before{0,1,true,{{"position",AttributeStruct{{"x",0.0},{"y",0.0},{"z",0.0}}}}};
        auto after=before; after.revision=2; std::get<AttributeStruct>(after.values.at("position")).at("x")=1.0;
        auto full=encode_attributes({1,2,false,after.values}); auto update=attribute_delta(before,after); auto delta=encode_attributes(update);
        AttributeReplica replica; require(replica.apply(before) && replica.apply(delta) && replica.values()==after.values,"position equivalence");
        std::cout<<"{\"scenario\":\"position_x\",\"full_fsa_bytes\":"<<full.size()<<",\"patch_fsa_bytes\":"<<delta.size()<<",\"patches\":"<<update.patches.size()<<"}\n";
    }
    for(auto name:{"list_replace","list_append16","map_add16","map_erase16"}) {
        AttributeUpdate before{0,1,true,{{"items",std::string(name).starts_with("map")?AttributeValue(map):AttributeValue(list)}}};
        auto after=before; after.revision=2;
        std::string mode=name;
        if(mode=="list_replace") std::get<AttributeStruct>(std::get<AttributeArray>(after.values.at("items"))[64]).at("count")=std::int64_t(2);
        if(mode=="list_append16") for(int i=0;i<16;++i) std::get<AttributeArray>(after.values.at("items")).emplace_back(AttributeStruct{{"id",std::string("added-")+std::to_string(i)},{"count",std::int64_t(1)}});
        if(mode=="map_add16") for(int i=0;i<16;++i) std::get<AttributeMap>(after.values.at("items"))["added-"+std::to_string(i)]=AttributeStruct{{"id",std::string("added-")+std::to_string(i)},{"count",std::int64_t(1)}};
        if(mode=="map_erase16") for(int i=0;i<16;++i) std::get<AttributeMap>(after.values.at("items")).erase(std::to_string(i));
        auto old=encode_attributes({1,2,false,after.values}); auto update=attribute_delta(before,after); auto compressed=encode_attributes(update);
        AttributeReplica replica; require(replica.apply(before) && replica.apply(compressed) && replica.values()==after.values,"wire equivalence");
        std::cout<<"{\"scenario\":\""<<mode<<"\",\"full_fsa_bytes\":"<<old.size()<<",\"patch_fsa_bytes\":"<<compressed.size()<<",\"patches\":"<<update.patches.size()<<"}\n";
    }
}
int main(int argc,char** argv) {
    try {
#ifndef NDEBUG
        throw std::runtime_error("use a Release build for performance comparison");
#endif
        std::string mode=argc>1?argv[1]:"all"; auto rounds=argc>2?std::stoull(argv[2]):100;
        require(rounds>=10 && rounds<=10000,"rounds must be 10..10000");
        if(mode=="all" || mode=="wire") wire();
        if(mode=="all" || mode=="sequential") network(false,rounds);
        if(mode=="all" || mode=="batch") network(true,rounds);
        require(mode=="all" || mode=="wire" || mode=="batch" || mode=="sequential","mode must be all/wire/sequential/batch");
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n'; return 1;}
}

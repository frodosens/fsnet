#include "fsnet/lua_engine.hpp"
#include "fsnet/entity_host.hpp"
#include "fsnet/attribute_replication.hpp"
#include "fsnet/entity_message.hpp"
#include <algorithm>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <optional>
#include <thread>
#include <unordered_map>
#include <vector>
using Clock=std::chrono::steady_clock;
using namespace std::chrono_literals;
namespace {
void require(bool value,const char* message) { if(!value) throw std::runtime_error(message); }
void result(const std::string& name,std::size_t n,Clock::time_point begin,std::size_t wire=0,std::size_t recipients=0,std::vector<double> latency={}) {
    auto seconds=std::chrono::duration<double>(Clock::now()-begin).count();
    std::sort(latency.begin(),latency.end());
    std::cout<<std::setprecision(10)<<"{\"scenario\":\""<<name<<"\",\"iterations\":"<<n<<",\"seconds\":"<<seconds
        <<",\"ns_per_op\":"<<seconds*1e9/n<<",\"wire_bytes\":"<<wire<<",\"recipients\":"<<recipients;
    if(!latency.empty()) std::cout<<",\"p50_us\":"<<latency[latency.size()/2]<<",\"p99_us\":"<<latency[(latency.size()-1)*99/100];
    std::cout<<"}\n";
}
fsnet::EntitySchema schema() {
    fsnet::EntitySchema s; s.legacy_owner_from_id=true;
    fsnet::AttributeDefinition vec{fsnet::AttributeType::structure,fsnet::AttributeStruct{{"x",0.0},{"y",0.0},{"z",0.0}}};
    for(auto key:{"x","y","z"}) vec.fields.emplace(key,fsnet::AttributeDefinition{fsnet::AttributeType::number,0.0});
    vec.visibility=fsnet::Visibility::observers; vec.driver_authority=true; s.attributes.emplace("position",vec);
    fsnet::AttributeDefinition number{fsnet::AttributeType::number,0.0};
    fsnet::AttributeDefinition row{fsnet::AttributeType::array,fsnet::AttributeArray(4,0.0)};
    row.element=std::make_shared<const fsnet::AttributeDefinition>(number); row.length=4;
    fsnet::AttributeDefinition matrix{fsnet::AttributeType::array,fsnet::AttributeArray(4,row.initial)};
    matrix.element=std::make_shared<const fsnet::AttributeDefinition>(row); matrix.length=4; matrix.visibility=fsnet::Visibility::observers;
    s.attributes.emplace("matrix",matrix);
    for(int i=0;i<30;++i) {
        fsnet::AttributeDefinition d{fsnet::AttributeType::integer,std::int64_t(0)};
        d.visibility=i<20?fsnet::Visibility::observers:fsnet::Visibility::owner;
        s.attributes.emplace("p"+std::to_string(i),d);
    }
    return s;
}
std::string lua_model() {
    std::string s=R"(
        local E=fsnet.Entity:extend('Perf',{owner='legacy_id'})
        E:attr('position',{type='vec3',authority='driver',sync='observers'})
        E:attr('matrix',{type='mat4x4',sync='observers'})
    )";
    for(int i=0;i<30;++i) s+="E:attr('p"+std::to_string(i)+"',{type='int64',default=0,sync='"+(i<20?"observers":"owner")+"'})\n";
    return s;
}
void native(std::size_t n) {
    fsnet::EntityAttributes model(schema(),"alice"); auto begin=Clock::now();
    for(std::size_t i=1;i<=n;++i) require(model.publish(model.stage({{"p0",static_cast<std::int64_t>(i)}},{fsnet::Caller::server,{}})),"scalar publish");
    require(model.revision()==n,"scalar checksum"); result("publish_scalar_32",n,begin);
    begin=Clock::now();
    for(std::size_t i=1;i<=n;++i) require(model.publish(model.stage({{"position",fsnet::AttributeStruct{{"x",double(i)},{"y",0.0},{"z",0.0}}}},{fsnet::Caller::server,{}})),"struct publish");
    require(model.revision()==2*n,"struct checksum"); result("publish_vec3_32",n,begin);
    fsnet::AttributeReplica replica; require(replica.apply(model.snapshot({"alice",true})),"replica snapshot");
    auto rev=replica.revision(); begin=Clock::now();
    for(std::size_t i=1;i<=n;++i) require(replica.apply({rev+i-1,rev+i,false,{{"position",fsnet::AttributeStruct{{"x",double(i)},{"y",0.0},{"z",0.0}}}}}),"replica delta");
    require(replica.revision()==rev+n,"replica checksum"); result("replica_delta_32",n,begin);
    auto image=model.snapshot({"alice",true}); auto size=fsnet::encode_attributes(image).size();
    auto rounds=std::max<std::size_t>(1000,n/4); std::size_t checksum=0; begin=Clock::now();
    for(std::size_t i=0;i<rounds;++i) { auto bytes=fsnet::encode_attributes(image); auto decoded=fsnet::decode_attributes(bytes); checksum+=decoded.values.size(); }
    require(checksum==32*rounds,"codec checksum"); result("codec_roundtrip_32",rounds,begin,size);
    auto delta=fsnet::encode_attributes({0,1,false,{{"position",fsnet::AttributeStruct{{"x",1.0},{"y",0.0},{"z",0.0}}}}});
    std::cout<<"{\"scenario\":\"payload_sizes\",\"full_bytes\":"<<size<<",\"vec3_delta_bytes\":"<<delta.size()<<"}\n";
}
struct LocalHost final:fsnet::ScriptHost {
    std::unordered_map<std::string,fsnet::Handler> routes;
    bool has_route(const std::string& name) const override { return routes.contains(name); }
    void bind(std::string name,fsnet::Handler handler) override { routes.emplace(std::move(name),std::move(handler)); }
    void unbind(const std::string& name) override { routes.erase(name); }
    std::uint64_t connect(const std::string&,std::uint16_t) override { throw std::logic_error("local benchmark cannot connect"); }
    void call(std::uint64_t,std::string,std::string,std::chrono::milliseconds,fsnet::Reply) override { throw std::logic_error("local benchmark cannot call"); }
};
void lua(std::size_t n) {
    for(const auto& mode:{"nested_read","whole_write","nested_write"}) {
        auto host=std::make_shared<LocalHost>(); fsnet::ScriptLimits limits; limits.instructions_per_resume=100000000;
        fsnet::LuaEngine engine(host,limits);
        std::string body=std::string(mode)=="nested_read"?"local sum=0; for i=1,n do sum=sum+entity.position.x end; assert(sum==0)":
            std::string(mode)=="whole_write"?"for i=1,n do entity.position={x=i,y=0,z=0} end; assert(entity.position.x==n)":
            "for i=1,n do entity.position.x=i end; assert(entity.position.x==n)";
        engine.load(lua_model()+"E:define(); local entity=E:spawn_local(); runtime.legacy_rpc('run',function(payload) local n=tonumber(payload); "+body+"; return 'done' end)");
        std::optional<fsnet::Result> reply; auto begin=Clock::now();
        host->routes.at("run")(std::to_string(n),[&](fsnet::Result value) { reply=std::move(value); });
        while(!reply) { engine.poll(); require(Clock::now()-begin<30s,"Lua benchmark deadline"); }
        if(reply->status!=fsnet::Status::ok) throw std::runtime_error(engine.last_error());
        result(std::string("lua_")+mode+"_32",n,begin); engine.stop(); require(engine.memory_used()==0,"Lua cleanup");
    }
}
void fanout(std::size_t n,std::size_t count) {
    fsnet::Node server("attribute-benchmark"); std::optional<std::uint64_t> owner;
    server.identify([&](std::uint64_t peer,const std::string&) {
        if(!owner) owner=peer;
        return std::optional(fsnet::CallIdentity{fsnet::Caller::client,peer==*owner?"alice":"viewer"});
    });
    auto transport=std::make_shared<fsnet::NodeScriptHost>(server);
    auto host=std::make_shared<fsnet::EntityScriptHost>(transport,fsnet::memory_database(),"benchmark/boot");
    fsnet::AttributeReplication replication(server,host,[](const auto&,const auto&,const auto&){ return true; });
    fsnet::LuaEngine engine(host);
    engine.load(lua_model()+R"(
        E:rpc('step',{callers={'client'},access='owner'})
        function E:step(payload) self.position.x=assert(tonumber(payload)) end
        E:register()
    )");
    auto port=server.listen("127.0.0.1",0);
    std::vector<std::unique_ptr<fsnet::Node>> nodes;
    std::vector<std::unique_ptr<fsnet::AttributeClient>> clients;
    std::vector<std::uint64_t> peers; std::vector<double> positions(count,0.0); std::size_t ready=0;
    auto pump=[&] {
        server.poll(); for(auto& node:nodes) node->poll(); host->poll(); replication.poll(); engine.poll();
        for(auto& client:clients) client->poll();
    };
    auto until=[&](auto done) {
        auto deadline=Clock::now()+10s;
        while(!done()) { pump(); require(Clock::now()<deadline,"network benchmark deadline"); }
    };
    for(std::size_t i=0;i<count;++i) {
        nodes.push_back(std::make_unique<fsnet::Node>("attribute-benchmark")); peers.push_back(nodes.back()->connect("127.0.0.1",port));
        clients.push_back(std::make_unique<fsnet::AttributeClient>(*nodes.back(),peers.back(),[&,i](std::uint64_t,fsnet::Status status,const fsnet::AttributeReplica* value) {
            require(status==fsnet::Status::ok && value,"benchmark subscription revoked");
            positions[i]=std::get<double>(std::get<fsnet::AttributeStruct>(value->values().at("position")).at("x"));
        }));
        clients.back()->watch("Perf","alice",[&](fsnet::Status status,std::uint64_t) { require(status==fsnet::Status::ok,"watch benchmark failed"); ++ready; });
        until([&]{return ready==i+1;}); // authenticate the writer first
    }
    std::vector<double> latency; latency.reserve(n);
    // Warm up all paths without including connection establishment in timing.
    auto advance=[&](std::size_t value) {
        bool ack=false; auto start=Clock::now();
        nodes.front()->call(peers.front(),"Perf.step",fsnet::entity_message("alice",std::to_string(value)),5s,[&](fsnet::Result reply) { require(reply.status==fsnet::Status::ok,"step benchmark failed"); ack=true; });
        until([&]{return ack && std::all_of(positions.begin(),positions.end(),[&](double x){return x==double(value);});});
        return std::chrono::duration<double,std::micro>(Clock::now()-start).count();
    };
    for(std::size_t i=1;i<=20;++i) advance(i);
    auto begin=Clock::now();
    for(std::size_t i=1;i<=n;++i) latency.push_back(advance(i+20));
    // FSA1 vec3 delta + FSN1 identity/lease/subscription envelope. TCP framing is
    // deliberately excluded; this is application payload, not captured NIC bytes.
    auto wire=fsnet::encode_attributes({0,1,false,{{"position",fsnet::AttributeStruct{{"x",1.0},{"y",0.0},{"z",0.0}}}}}).size()+4+1+8+4+1+4+2+5;
    result("fanout_"+std::to_string(count),n,begin,wire,count,std::move(latency));
    for(auto& client:clients) client->stop(); engine.stop(); replication.stop(); host->stop();
}
}
int main(int argc,char** argv) {
    try {
#ifdef NDEBUG
        std::cout<<"{\"scenario\":\"build\",\"mode\":\"Release(NDEBUG)\",\"compiler\":\""<<__VERSION__<<"\"}\n";
#else
        std::cout<<"{\"scenario\":\"build\",\"mode\":\"Debug\",\"compiler\":\""<<__VERSION__<<"\"}\n";
#endif
        std::string mode=argc>1?argv[1]:"all";
        auto n=argc>2?std::stoull(argv[2]):20000ULL;
        require(n>=100 && n<=1000000,"iterations must be 100..1000000");
        if(mode=="all" || mode=="native") native(n);
        if(mode=="all" || mode=="lua") lua(n);
        if(mode=="all" || mode=="network") for(auto count:{1U,8U,32U}) fanout(std::max<std::size_t>(100,n/20),count);
        require(mode=="all" || mode=="native" || mode=="lua" || mode=="network","mode must be all/native/lua/network");
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}

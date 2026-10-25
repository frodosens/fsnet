#pragma once
#include "fsnet/entity_host.hpp"
#include "fsnet/entity_message.hpp"
#include "fsnet/entity_record.hpp"
#include "fsnet/lua_engine.hpp"
#include <thread>
#include <unordered_map>
namespace testing {
using namespace std::chrono_literals;
inline void require(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
struct Parent:fsnet::ScriptHost {
    std::unordered_map<std::string,fsnet::ContextHandler> routes;
    bool has_route(const std::string& name)const override{return routes.contains(name);}
    void bind(std::string,fsnet::Handler)override{throw std::logic_error("context required");}
    void bind_context(std::string name,fsnet::RpcPolicy,fsnet::ContextHandler fn)override{routes.emplace(std::move(name),std::move(fn));}
    void unbind(const std::string& name)override{routes.erase(name);}
    std::uint64_t connect(const std::string&,std::uint16_t)override{return 0;}
    void call(std::uint64_t,std::string,std::string,std::chrono::milliseconds,fsnet::Reply reply)override{reply({fsnet::Status::not_found,{}});}
};
struct Runtime {
    std::shared_ptr<fsnet::Storage> db=fsnet::memory_database();
    std::shared_ptr<Parent> parent=std::make_shared<Parent>();
    std::shared_ptr<fsnet::EntityScriptHost> host;
    std::unique_ptr<fsnet::LuaEngine> vm;
    Runtime(std::string source,fsnet::ScriptLimits limits={}) {
        host=std::make_shared<fsnet::EntityScriptHost>(parent,db,"test/boot");
        vm=std::make_unique<fsnet::LuaEngine>(host,limits);vm->load(source,"current-runtime");
    }
    ~Runtime(){vm->stop();host->stop();}
    void poll(){host->poll();vm->poll();}
    template<class F> void until(F done){auto end=std::chrono::steady_clock::now()+5s;while(!done()){poll();if(std::chrono::steady_clock::now()>end)throw std::runtime_error("runtime deadline: "+vm->last_error());std::this_thread::sleep_for(1ms);}}
    template<class F> fsnet::Result wait(F begin){std::optional<fsnet::Result> result;begin([&](auto value){result=std::move(value);});until([&]{return result.has_value();});return *result;}
    fsnet::EntityId create(const std::string& type,const std::string& key,fsnet::ConnectionRef owner={},fsnet::EntityAttributes::Values values={}) {
        auto value=wait([&](auto reply){host->create_entity(type,std::move(values),{"tests",key,std::move(owner),"test"},std::move(reply));});
        require(value.status==fsnet::Status::ok,"entity creation failed");return fsnet::EntityId(value.data);
    }
    void begin(fsnet::EntityId id,std::string_view method,std::string_view data,fsnet::Reply reply,
               fsnet::CallIdentity who={fsnet::Caller::server,"test"},std::string_view receipt={}) {
        parent->routes.at("@")(who,fsnet::entity_call_message(id,method,data,receipt),std::move(reply));
    }
    fsnet::Result call(fsnet::EntityId id,std::string_view method,std::string_view data={},fsnet::CallIdentity who={fsnet::Caller::server,"test"},std::string_view receipt={}) {
        return wait([&](auto reply){begin(id,method,data,std::move(reply),who,receipt);});
    }
    fsnet::EntityRecord record(fsnet::EntityId id){return fsnet::decode_entity_record(db->get(fsnet::entity_record_key(id))->value);}
    fsnet::AttributeUpdate image(fsnet::EntityId id){return fsnet::decode_attributes(record(id).snapshot.state);}
};
}

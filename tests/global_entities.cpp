#include "fsnet/entity_host.hpp"
#include "fsnet/entity_message.hpp"
#include <iostream>
#include <thread>
#include <unordered_map>
using namespace fsnet;
using namespace std::chrono_literals;
#define CHECK(c) do{if(!(c))throw std::runtime_error(std::string(__FILE__)+":"+std::to_string(__LINE__)+" " #c);}while(false)
struct Host final:ScriptHost {
    std::unordered_map<std::string,Handler> handlers;
    std::unordered_map<std::string,ContextHandler> contexts;
    bool has_route(const std::string& name)const override{return handlers.contains(name)||contexts.contains(name);}
    void bind(std::string name,Handler handler)override{CHECK(!has_route(name));handlers.emplace(name,std::move(handler));}
    void bind_context(std::string name,RpcPolicy,ContextHandler handler)override{CHECK(!has_route(name));contexts.emplace(name,std::move(handler));}
    void unbind(const std::string& name)override{handlers.erase(name);contexts.erase(name);}
    std::uint64_t connect(const std::string&,std::uint16_t)override{return 1;}
    void call(std::uint64_t,std::string,std::string,std::chrono::milliseconds,Reply reply)override{reply({Status::not_found,{}});}
};
template<class F> Result wait(EntityScriptHost& runtime,F start){
    std::optional<Result> result;unsigned count=0;start([&](Result value){CHECK(++count==1);result=std::move(value);});
    auto deadline=std::chrono::steady_clock::now()+3s;
    while(!result){runtime.poll();CHECK(std::chrono::steady_clock::now()<deadline);std::this_thread::sleep_for(1ms);}return std::move(*result);
}
void define(EntityScriptHost& runtime,const std::string& type){
    EntitySchema schema;AttributeDefinition value;value.type=AttributeType::integer;value.initial=std::int64_t(0);schema.attributes.emplace("count",value);
    runtime.define_schema(type,schema);EntityAttributes initial(schema,"");auto state=encode_attributes({0,0,true,initial.persistent()});
    runtime.bind_stateful_context(type+".increment",type,state,{true,true,true},[](const CallIdentity&,std::string input,Reply reply){
        auto entity=parse_entity_message(input);auto state=parse_state_message(entity->payload);auto image=decode_attributes(state->state);
        auto next=std::get<std::int64_t>(image.values.at("count"))+1;image.values["count"]=next;++image.revision;
        reply({Status::ok,state_message(encode_attributes(image),std::to_string(next))});
    });
    runtime.bind_stateful_context(type+".$destroy",type,state,{false,true,false},[](const CallIdentity&,std::string input,Reply reply){
        auto entity=parse_entity_message(input);auto state=parse_state_message(entity->payload);reply({Status::ok,state_message(state->state,"destroyed")});
    });
}
int main(){try{
    auto db=memory_database();auto parent=std::make_shared<Host>();std::int64_t now=10000;
    EntityScriptHost runtime(parent,db,"node-a/boot-1",{},{},[&]{return now;});define(runtime,"Player");define(runtime,"LoginEntity");
    EntitySpawnOptions options{"manager-1","player-1","account-1","node-a"};
    auto created=wait(runtime,[&](Reply reply){runtime.create_entity("Player",{},options,std::move(reply));});CHECK(created.status==Status::ok);auto id=EntityId(created.data);
    auto call=[&](EntityId target,std::string subject,std::string expected={}){return wait(runtime,[&](Reply reply){parent->contexts.at("@")({Caller::client,subject,1},entity_call_message(target,"increment","","",expected),std::move(reply));});};
    CHECK(call(id,"account-1").data=="1");CHECK(call(id,"account-2").status==Status::rejected);
    CHECK(call(id,"account-1","LoginEntity").status==Status::rejected);CHECK(call(EntityId(UINT64_MAX),"account-1").status==Status::not_found);
    CHECK(!db->get(entity_record_key(EntityId(UINT64_MAX))));CHECK(call(id,"account-1").data=="2");
    auto retry=wait(runtime,[&](Reply reply){runtime.create_entity("Player",{},options,std::move(reply));});CHECK(retry.data==id.str());
    auto conflict=options;conflict.owner_account_id="account-2";CHECK(wait(runtime,[&](Reply reply){runtime.create_entity("Player",{},conflict,std::move(reply));}).status==Status::rejected);
    options.creation_key="login-1";options.persistence=EntityPersistence::rebuildable;
    auto login=wait(runtime,[&](Reply reply){runtime.create_entity("LoginEntity",{},options,std::move(reply));});CHECK(login.status==Status::ok);auto login_id=EntityId(login.data);
    CHECK(call(login_id,"account-1").data=="1");CHECK(call(login_id,"account-1").data=="2");
    auto encoded=entity_call_message(id,"increment","","token","Player");for(std::size_t n=0;n<encoded.size();++n)CHECK(!parse_entity_call_view(std::string_view(encoded).substr(0,n)));
    runtime.stop();now+=120000;auto second_parent=std::make_shared<Host>();EntityScriptHost second(second_parent,db,"node-a/boot-2",{},{},[&]{return now;});define(second,"Player");define(second,"LoginEntity");
    auto second_call=[&](EntityId target){return wait(second,[&](Reply reply){second_parent->contexts.at("@")({Caller::client,"account-1",1},entity_call_message(target,"increment",""),std::move(reply));});};
    CHECK(second_call(id).data=="3");CHECK(second_call(login_id).data=="1");
    CHECK(wait(second,[&](Reply reply){second.control_entity("Player",id.str(),true,std::move(reply));}).status==Status::ok);
    CHECK(second_call(id).status==Status::not_found);CHECK(decode_entity_record(db->get(entity_record_key(id))->value).metadata->destroyed);
    options={"manager-1","player-1","account-1","node-a"};CHECK(wait(second,[&](Reply reply){second.create_entity("Player",{},options,std::move(reply));}).status==Status::not_found);
    second.stop();std::cout<<"PASS global entities: metadata dispatch, account authority, explicit creation, rebuild, restart and tombstones\n";
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}

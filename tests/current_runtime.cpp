#include "support/runtime.hpp"
#include <iostream>
using namespace fsnet;using namespace testing;
const char* source=R"lua(
Actor=fsnet.Entity:extend('Actor')
Actor:attr('x',{type='int32',default=0})
Actor:attr('created',{type='int32',default=0})
Actor:attr('activated',{type='int32',default=0})
Actor:attr('label',{type='string',default=''})
Actor:attr('ticks',{type='int32',default=0})
function Actor:on_create() self.created=self.created+1 end
function Actor:on_activate() self.activated=self.activated+1 end
function Actor:on_tick() self.ticks=self.ticks+1 end
Actor:rpc('add',{callers={'server'}})
function Actor:add(value) self.x=self.x+assert(tonumber(value));return tostring(self.x) end
Actor:rpc('owner_add',{callers={'client'},access='owner'})
function Actor:owner_add(value) return self:add(value) end
Actor:rpc('read',{callers={'client','server'}})
function Actor:read() return tostring(self.x) end
Actor:rpc('fail',{callers={'server'}})
function Actor:fail() self.x=999;error('rollback') end
Actor:rpc('wait_owner',{callers={'client'},access='owner'})
function Actor:wait_owner() self.x=999;runtime.sleep(30);return 'done' end
Actor:rpc('wait_server',{callers={'server'}})
function Actor:wait_server() self.x=999;runtime.sleep(30);return 'done' end
Actor:rpc('child',{callers={'server'}})
function Actor:child(key)
    local child=Actor:spawn({},{creation_key=key})
    child:add('3')
    return child.id
end
Actor:register()
)lua";
int main(){try{
    Runtime runtime(source);auto alice=ConnectionRef::create(),bob=ConnectionRef::create();
    auto id=runtime.create("Actor","one",alice);auto repeated=runtime.create("Actor","one",alice);require(id==repeated,"creation retry changed identity");
    require(!runtime.parent->has_route("Actor.add"),"typed transport route exposed");
    CallIdentity owner{Caller::client,"same-account",1,alice},other{Caller::client,"same-account",2,bob};
    require(runtime.call(id,"add","1",owner).status==Status::rejected,"client reached backend method");
    require(runtime.call(id,"owner_add","1",other).status==Status::rejected,"other connection inherited ownership");
    require(runtime.call(id,"owner_add","2",owner).data=="2","owner method failed");
    require(runtime.call(id,"read","",other).data=="2","public method failed");
    require(runtime.call(id,"add","5",{Caller::server,"test"},"once").data=="7","receipt initial call");
    require(runtime.call(id,"add","5",{Caller::server,"test"},"once").data=="7","receipt replay");
    require(runtime.call(id,"add","6",{Caller::server,"test"},"once").status==Status::rejected,"receipt accepted different input");
    require(runtime.call(id,"fail").status!=Status::ok && runtime.call(id,"read").data=="7","failed state committed");
    bool live=true;owner.current=[&]{return live;};std::optional<Result> pending;
    runtime.begin(id,"wait_owner","",[&](Result value){pending=std::move(value);},owner);
    runtime.until([&]{return runtime.vm->active()>0;});live=false;
    runtime.until([&]{return pending.has_value();});require(pending->status==Status::rejected,"revoked suspended operation committed");
    require(runtime.call(id,"read").data=="7","revoked state published");
    auto child=runtime.call(id,"child","child");require(child.status==Status::ok && valid_entity_id(child.data),"entity child creation");
    require(runtime.call(EntityId(child.data),"read").data=="3","child call");
    runtime.until([&]{return std::get<std::int64_t>(runtime.image(id).values.at("ticks"))>0;});
    auto before=runtime.image(id);require(std::get<std::int64_t>(before.values.at("created"))==1,"create hook repeated");
    runtime.until([&]{return runtime.host->stats().pending==0;});
    auto off=runtime.wait([&](auto reply){runtime.host->control_entity("Actor",id.str(),false,std::move(reply));});require(off.status==Status::ok,"deactivate");
    require(runtime.call(id,"read").data=="7","state lost after activation");
    auto after=runtime.image(id);require(std::get<std::int64_t>(after.values.at("created"))==1 && std::get<std::int64_t>(after.values.at("activated"))>=2,"lifecycle hooks");
    auto destroyed=runtime.wait([&](auto reply){runtime.host->control_entity("Actor",id.str(),true,std::move(reply));});require(destroyed.status==Status::ok,"destroy");
    require(runtime.call(id,"read").status==Status::not_found,"destroyed identity resurrected");
    require(runtime.call(EntityId(UINT64_MAX),"read").status==Status::not_found,"unknown identity implicitly created");
    {
        Runtime fenced(source);auto target=fenced.create("Actor","fence");std::optional<Result> stale;
        fenced.begin(target,"wait_server","",[&](Result value){stale=std::move(value);});
        fenced.until([&]{return fenced.vm->active()>0;});auto key=entity_record_key(target);
        auto old=fenced.db->get(key);auto changed=decode_entity_record(old->value);changed.owner="other/boot";++changed.fence;
        require(fenced.db->compare_exchange(key,old->version,encode_entity_record(changed)),"takeover fixture CAS");
        fenced.until([&]{return stale.has_value();});require(stale->status==Status::rejected,"stale owner committed");
        require(std::get<std::int64_t>(fenced.image(target).values.at("x"))==0,"stale state published");
    }
    {
        Runtime unsupported(source);auto target=unsupported.create("Actor","retired-record");
        auto key=entity_record_key(target);auto saved=unsupported.db->get(key);auto record=decode_entity_record(saved->value);
        std::string retired="FER1";snapshot_detail::blob(retired,record.owner);
        entity_record_detail::integer(retired,record.expires_ms);entity_record_detail::integer(retired,record.fence);
        retired+=encode_snapshot(record.snapshot);
        require(unsupported.db->compare_exchange(key,saved->version,retired),"retired record fixture");
        auto before=unsupported.db->get(key);
        require(unsupported.call(target,"read").status!=Status::ok,"unsupported record accepted");
        auto after=unsupported.db->get(key);
        require(after->version==before->version && after->value==before->value,"unsupported record reset or overwritten");
    }
    std::cout<<"PASS current runtime: global creation, caller/connection grants, receipts, rollback, suspension revocation, nested calls and lifecycle\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}

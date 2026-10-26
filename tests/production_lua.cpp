#include "support/runtime.hpp"
#include <cstdlib>
#include <iostream>
#include <new>
#include <unordered_set>

// Count only owner-thread C++ allocation requests during the rejected wide
// mutation. Lua's allocator has a separate quota; the regression was expansion
// into tens of MiB of std::string copies before that quota was consulted.
thread_local bool count_native_allocations=false;
thread_local std::size_t native_allocated_bytes=0;
void* operator new(std::size_t size) {
    if(count_native_allocations)native_allocated_bytes+=size;
    if(auto* result=std::malloc(size?size:1))return result;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer,std::size_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer,std::size_t) noexcept { std::free(pointer); }

using namespace fsnet;
using namespace testing;

template<class Predicate> void poll_until(LuaEngine& vm,Predicate done) {
    const auto deadline=std::chrono::steady_clock::now()+2s;
    while(!done()) {
        vm.poll();require(std::chrono::steady_clock::now()<deadline,"Lua regression deadline");
        std::this_thread::sleep_for(1ms);
    }
}

struct WatchHost:Parent {
    std::uint64_t serial=0;
    unsigned releases=0;
    std::unordered_set<std::uint64_t> live;
    std::shared_ptr<const AttributeUpdate> image=std::make_shared<const AttributeUpdate>(AttributeUpdate{0,1,true,{{"x",std::int64_t(0)}}});
    void watch_attributes(std::string,std::string,AttributeReady reply,std::chrono::milliseconds)override {
        live.insert(++serial);reply(Status::ok,serial);
    }
    std::shared_ptr<const AttributeUpdate> attribute_snapshot(std::uint64_t id)const override {
        return live.contains(id)?image:nullptr;
    }
    void unwatch_attributes(std::uint64_t id)override { if(live.erase(id))++releases; }
};

void finalizers() {
    auto host=std::make_shared<Parent>();ScriptLimits limits;limits.memory_bytes=512*1024;
    LuaEngine vm(host,limits);
    vm.load(R"lua(
        local callback=function() while true do end end
        assert(not pcall(setmetatable,{}, {__gc=callback}))
        assert(not pcall(setmetatable,{}, {__gc=false}))
        local mt={};local first=setmetatable({},mt)
        rawset(mt,'__gc',callback)
        assert(not pcall(setmetatable,first,mt))
        assert(not pcall(setmetatable,{},mt))
        -- Adding a finalizer after attachment does not register the table for
        -- finalization; reattaching that metatable is checked above.
        first=nil;mt=nil
        for i=1,256 do local garbage=string.rep('x',4096) end
        assert(getmetatable(setmetatable({}, {__metatable=false}))==false)
    )lua","finalizer-policy");
    require(vm.startup_complete(),"load without starts did not become ready");
    vm.stop();require(vm.memory_used()==0,"native/VM charges survived stop");
    {
        LuaEngine failed(host,limits);bool rejected=false;
        try { failed.load("setmetatable({}, {__gc=function() while true do end end})"); }
        catch(const std::exception&) { rejected=true; }
        require(rejected && failed.startup_failed(),"load accepted a script finalizer");
        require(failed.memory_used()==0,"failed load did not release VM");
    }
    {
        auto watches=std::make_shared<WatchHost>();LuaEngine resources(watches,limits);
        resources.load(R"lua(
            local Actor=fsnet.Entity:extend('Actor')
            Actor:attr('x',{type='int32',default=0});Actor:define()
            runtime.start(function()
                local unused=Actor:watch('000000000000002a');unused=nil
                for i=1,256 do local garbage=string.rep('x',4096) end
                kept=Actor:watch('000000000000002a')
            end)
        )lua","native-finalizers");
        poll_until(resources,[&]{return resources.startup_complete() || resources.startup_failed();});
        require(resources.startup_complete(),"native finalizer setup failed");
        require(watches->releases==1 && watches->live.size()==1,"ordinary GC did not release native watch");
        resources.stop();require(watches->releases==2 && watches->live.empty(),"stop skipped native watch finalizer");
        require(resources.memory_used()==0,"native finalizer charges not returned");
    }
}

void native_memory() {
    ScriptLimits limits;limits.memory_bytes=2*1024*1024;
    Runtime runtime(R"lua(
        local Actor=fsnet.Entity:extend('Actor')
        Actor:attr('x',{type='int32',default=0})
        Actor:attr('rows',{type=fsnet.list(fsnet.array('string',256),4)})
        Actor:rpc('wide',{callers={'server'}})
        function Actor:wide()
            self.x=99
            local value=string.rep('x',65536)
            local row={} for i=1,256 do row[i]=value end
            self.rows={row,row,row,row}
            return 'bad'
        end
        Actor:rpc('healthy',{callers={'server'}})
        function Actor:healthy() self.x=self.x+1;return tostring(self.x) end
        Actor:register()
    )lua",limits);
    auto id=runtime.create("Actor","native-value-budget");
    native_allocated_bytes=0;count_native_allocations=true;
    auto rejected=runtime.call(id,"wide");
    count_native_allocations=false;
    require(rejected.status==Status::overloaded,"wide native expansion did not report resource exhaustion");
    require(native_allocated_bytes<4*1024*1024,"wide alias graph expanded before budget check");
    require(std::get<std::int64_t>(runtime.image(id).values.at("x"))==0,"rejected native candidate committed");
    require(runtime.call(id,"healthy").data=="1","native budget failure did not recover");
    require(runtime.vm->memory_used()<=limits.memory_bytes,"temporary native charges leaked");
    {
        auto parent=std::make_shared<Parent>();LuaEngine vm(parent,limits);bool failed=false;
        native_allocated_bytes=0;count_native_allocations=true;
        try { vm.load(R"lua(
            local Actor=fsnet.Entity:extend('Actor')
            local shared=string.rep('x',65536)
            for i=1,64 do Actor:attr('field'..i,{type='string',default=shared}) end
            Actor:register()
        )lua","native-schema-budget"); }
        catch(const std::exception&) { failed=true; }
        count_native_allocations=false;
        require(failed && vm.startup_failed(),"schema aliases bypassed native budget");
        require(native_allocated_bytes<4*1024*1024,"schema expanded before native budget");
        require(vm.memory_used()==0,"rejected schema retained charges");
    }
}

struct WriteHost:WatchHost {
    LuaEngine* vm=nullptr;
    std::size_t during=0,charged=0;
    EntityAttributes::Values pending;
    Reply completion;
    WriteHost() { image=std::make_shared<const AttributeUpdate>(AttributeUpdate{0,1,true,{{"rows",AttributeArray{}}}}); }
    void write_attributes(std::uint64_t,EntityAttributes::Values values,Reply reply,AttributeWriteOptions,std::chrono::milliseconds)override {
        charged=1024+attribute_image_bytes(values);during=vm->memory_used();
        pending=std::move(values);completion=std::move(reply);
    }
};
void awaiting_values() {
    auto host=std::make_shared<WriteHost>();ScriptLimits limits;limits.memory_bytes=2*1024*1024;
    LuaEngine vm(host,limits);host->vm=&vm;
    vm.load(R"lua(
        local Actor=fsnet.Entity:extend('Actor')
        Actor:attr('rows',{type=fsnet.list('string',256),client='rw'})
        Actor:define()
        runtime.start(function()
            local confirmed=Actor:watch('000000000000002a')
            local value=string.rep('x',1024);local rows={}
            for i=1,100 do rows[i]=value end
            confirmed.rows=rows
            confirmed:close()
        end)
    )lua","await-native-charge");
    poll_until(vm,[&]{return bool(host->completion) || vm.startup_failed();});
    require(bool(host->completion),"confirmed write did not reach host");
    // Await retains its native allowance through the synchronous hand-off.
    // Thereafter the host owns pending values and its own queue budget applies.
    require(host->during>=vm.memory_used()+host->charged,"await dropped native values charge before host hand-off");
    host->pending.clear();auto completion=std::move(host->completion);completion({Status::ok,{}});
    poll_until(vm,[&]{return vm.startup_complete() || vm.startup_failed();});
    require(vm.startup_complete(),"charged write did not finish");vm.stop();
    require(vm.memory_used()==0,"await native reservation leaked");
}

void initialization() {
    auto host=std::make_shared<Parent>();
    {
        LuaEngine vm(host);require(!vm.startup_complete() && !vm.startup_failed(),"unloaded VM ready");
        vm.load("runtime.start(function() runtime.sleep(2) end);runtime.background(function() while true do runtime.sleep(1) end end)");
        require(!vm.startup_complete(),"queued initialization reported ready");
        poll_until(vm,[&]{return vm.startup_complete() || vm.startup_failed();});
        require(vm.startup_complete() && vm.active()==1,"background blocked successful initialization");
        vm.stop();require(!vm.startup_failed(),"background cancellation failed initialization");
    }
    {
        LuaEngine vm(host);vm.load("runtime.start(function() error('bootstrap fixture') end)");
        poll_until(vm,[&]{return vm.startup_failed();});
        require(!vm.startup_complete() && !vm.last_error().empty(),"failed initialization reported ready");
        vm.stop();require(vm.startup_failed(),"initialization failure not sticky");
    }
    {
        LuaEngine vm(host);vm.load("runtime.start(function() runtime.sleep(30000) end)");vm.poll();vm.stop();
        require(vm.startup_failed() && !vm.startup_complete(),"canceled initialization reported success");
    }
    {
        ScriptLimits limits;limits.task_timeout=15ms;LuaEngine vm(host,limits);
        vm.load("runtime.start(function() while true do runtime.sleep(2) end end)");
        poll_until(vm,[&]{return vm.startup_failed();});
        require(!vm.startup_complete(),"sleep refreshed finite initialization deadline");
    }
    {
        ScriptLimits limits;limits.task_timeout=50ms;limits.memory_bytes=512*1024;
        Runtime runtime(R"lua(
            local weak=setmetatable({}, {__mode='v'})
            do
                local retained={};weak[1]=retained
                runtime.start(function() assert(retained) end)
            end
            local Actor=fsnet.Entity:extend('Actor')
            Actor:rpc('check',{callers={'server'}})
            function Actor:check()
                for i=1,256 do local garbage=string.rep('x',4096) end
                return weak[1]==nil and 'released' or 'retained'
            end
            Actor:register()
        )lua",limits);
        std::this_thread::sleep_for(70ms);runtime.vm->poll();
        require(runtime.vm->startup_failed() && runtime.vm->active()==0,"queued initialization deadline not enforced");
        auto target=runtime.create("Actor","queued-initialization");
        require(runtime.call(target,"check").data=="released","expired queued function retained its captured object");
    }
}

void revoked_transactions() {
    auto parent=std::make_shared<Parent>();auto db=memory_database();
    EntityRuntimeLimits limits;limits.request_timeout=500ms;limits.lease=1000ms;
    auto host=std::make_shared<EntityScriptHost>(parent,db,"test/boot",limits);
    ScriptLimits scripts;scripts.task_timeout=2s;LuaEngine vm(host,scripts);
    vm.load(R"lua(
        local Actor=fsnet.Entity:extend('Actor')
        Actor:rpc('old',{callers={'server'}})
        function Actor:old(delay)
            runtime.sleep(tonumber(delay))
            self:after(60000,'noop','',{key='from-expired'})
            return 'old'
        end
        Actor:rpc('noop',{callers={'server'}})
        function Actor:noop()return 'healthy' end
        Actor:register()
    )lua","revoked-transactions");
    auto until=[&](auto done) {
        auto deadline=std::chrono::steady_clock::now()+3s;
        while(!done()) { host->poll();vm.poll();require(std::chrono::steady_clock::now()<deadline,"revoked task deadline");std::this_thread::sleep_for(1ms); }
    };
    std::optional<Result> created;
    host->create_entity("Actor",{},{"production-lua","revoked",{},"test"},[&](auto result){created=std::move(result);});
    until([&]{return created.has_value();});require(created->status==Status::ok,"revoked task entity creation");
    EntityId id(created->data);
    for(auto delay:{"100","1000"}) {
        std::optional<Result> old,next;
        host->call_entity("@",entity_call_message(id,"old",delay),20ms,[&](auto result){old=std::move(result);});
        until([&]{return old.has_value();});require(old->status==Status::timeout,"old transaction did not expire");
        host->call_entity("@",entity_call_message(id,"noop",""),400ms,[&](auto result){next=std::move(result);});
        until([&]{return next.has_value();});
        require(next->status==Status::ok && next->data=="healthy","expired Lua task blocked the next transaction");
        auto record=decode_entity_record(db->get(entity_record_key(id))->value);
        require(!record.snapshot.timers.entries.contains("from-expired"),"expired Lua task mutated a newer transaction");
        require(vm.active()==0,"revoked Lua coroutine remained resident");
    }
    vm.stop();host->stop();
}

int main() { try {
    finalizers();native_memory();awaiting_values();initialization();revoked_transactions();
    std::cout<<"PASS production Lua: explicit finalizer policy, native allocation preflight, recovery and initialization readiness\n";
} catch(const std::exception& error) { count_native_allocations=false;std::cerr<<error.what()<<'\n';return 1; } }

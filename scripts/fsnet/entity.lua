-- Built into the Lua adapter. No filesystem/package loader is required at runtime.
local classes = setmetatable({}, {__mode='k'})
local names = setmetatable({}, {__mode='v'})
local refs = setmetatable({}, {__mode='k'})
local Class, class_mt, ref_mt = {}, {}, {}
local register_entity=runtime.entity
local reserved = {id=true, type=true, context=true, reject=true, attr=true, rpc=true,
                  private=true, extend=true, register=true, ref=true, reference=true,
                  define=true, spawn_local=true, watch=true, write=true, close=true,
                  revision=true, mode=true, method=true, batch=true, prediction=true, spawn_or_restore=true,
                  deactivate=true, destroy=true, show_to=true, hide_from=true, possess=true, unpossess=true}
local unsupported_hooks = {new=true, init=true}
local lifecycle_hooks = {on_create=true,on_activate=true,on_deactivate=true,on_destroy=true}
local function name(value)
    assert(type(value)=='string' and #value>0 and #value<=127 and not value:find('[%.%z]') and value:sub(1,1)~='$', 'invalid entity/member name')
    return value
end
local function copy(value, seen, depth)
    if type(value)~='table' then return value end
    seen=seen or {}; depth=depth or 0
    assert(depth<32 and not seen[value], 'recursive or excessively nested definition')
    assert(getmetatable(value)==nil, 'definitions must use plain tables')
    seen[value]=true
    local out={}; local count=0
    for k,v in pairs(value) do
        count=count+1; assert(count<=256, 'definition field limit')
        assert(type(k)=='string' or type(k)=='number', 'invalid definition key')
        out[k]=copy(v,seen,depth+1)
    end
    seen[value]=nil
    return out
end
local function record(class)
    return assert(classes[class], 'expected an entity class')
end
local function editable(class)
    local data=record(class)
    assert(not data.sealed, 'entity class is sealed')
    return data
end
local function options(value)
    value=value or {}
    assert(type(value)=='table', 'options must be a table')
    for key in pairs(value) do assert(key=='owner', 'unknown entity option: '..tostring(key)) end
    assert(value.owner==nil or value.owner=='id' or value.owner=='external', 'invalid owner option')
    return copy(value)
end
local function policy(value)
    assert(type(value)=='table' and type(value.callers)=='table' and #value.callers>0, 'explicit RPC callers required')
    for key in pairs(value) do assert(key=='callers' or key=='access', 'unknown RPC option: '..tostring(key)) end
    local seen={}; local count=0
    for index,caller in pairs(value.callers) do
        assert(math.type(index)=='integer' and index>=1 and index<=#value.callers, 'callers must be a dense list')
        assert((caller=='client' or caller=='server') and not seen[caller], 'invalid or duplicate RPC caller')
        seen[caller]=true; count=count+1
    end
    assert(count==#value.callers, 'callers must be a dense list')
    assert(value.access==nil or value.access=='owner', 'invalid RPC access')
    return copy(value)
end
function Class:extend(kind, opts)
    record(self); name(kind)
    assert(not names[kind], 'entity class name already exists')
    local class=setmetatable({},class_mt)
    classes[class]={name=kind, parent=self, attrs={}, methods={}, rpcs={}, domains={}, options=options(opts)}
    names[kind]=class
    return class
end
function Class:attr(key, definition)
    local data=editable(self); name(key)
    assert(not reserved[key] and not unsupported_hooks[key] and not lifecycle_hooks[key] and key~='on_tick' and key~='validate_write', 'reserved property name')
    assert(data.attrs[key]==nil and type(definition)=='table', 'duplicate or invalid property')
    local allowed={type=true, default=true, server=true, client=true, sync=true, persist=true, authority=true}
    for option in pairs(definition) do assert(allowed[option], 'unknown property option: '..tostring(option)) end
    data.attrs[key]=copy(definition)
    return self
end
function Class:method(key,definition)
    local data=editable(self); name(key)
    assert(not reserved[key] and not unsupported_hooks[key], 'reserved method')
    assert(type(definition)=='table' and (definition.runs_on=='shared' or definition.runs_on=='server' or definition.runs_on=='client'), 'invalid execution domain')
    for option in pairs(definition) do assert(option=='runs_on','unknown method option') end
    assert(data.domains[key]==nil,'duplicate method domain')
    data.domains[key]=definition.runs_on
    return self
end
function Class:rpc(key, definition)
    local data=editable(self); name(key)
    assert(not reserved[key] and not lifecycle_hooks[key] and key~='on_tick' and key~='validate_write' and not unsupported_hooks[key], 'reserved RPC name')
    assert(data.rpcs[key]==nil, 'duplicate RPC declaration')
    data.rpcs[key]=policy(definition)
    return self
end
function Class:private(key)
    local data=editable(self); name(key)
    assert(data.rpcs[key]==nil, 'duplicate method visibility declaration')
    data.rpcs[key]=false
    return self
end
local function flatten(class)
    local chain={}
    while class do table.insert(chain,1,class); class=record(class).parent end
    local out={attrs={},methods={},rpcs={},domains={},options={}}
    for _,base in ipairs(chain) do
        local data=record(base)
        for key,value in pairs(data.options) do out.options[key]=value end
        for key,value in pairs(data.attrs) do out.attrs[key]=copy(value) end
        for key,value in pairs(data.domains) do out.domains[key]=value end
        for key,fn in pairs(data.methods) do
            assert(not out.rpcs[key] or data.rpcs[key]~=nil, 'overridden RPC requires an explicit policy: '..key)
            out.methods[key]=fn
        end
        for key,value in pairs(data.rpcs) do
            assert(value or out.methods[key], 'private method implementation missing: '..key)
            out.rpcs[key]=value and copy(value) or nil
        end
    end
    for key in pairs(out.attrs) do assert(not out.methods[key] and not reserved[key], 'property/method name collision: '..key) end
    for key in pairs(out.rpcs) do assert(out.methods[key], 'RPC implementation missing: '..key) end
    for key in pairs(out.domains) do assert(out.methods[key], 'method implementation missing: '..key) end
    return out,chain
end
local function check_domain(object,domain)
    runtime._object_check(object)
    assert(domain==nil or domain=='shared' or (domain=='server')==(object.mode=='server'), 'method execution domain denied')
end
local create_prediction
local function define(class)
    local data=record(class)
    if data.flat then return data.flat end
    local flat,chain=flatten(class)
    assert(data.name,'cannot define abstract root')
    runtime._attributes_define(data.name,flat.attrs,flat.options.owner=='id' and 'id' or nil,true)
    local members={}
    for key,fn in pairs(flat.methods) do
        members[key]=function(object,...)
            check_domain(object,flat.domains[key])
            return fn(object,...)
        end
    end
    members.prediction=function(object,opts) return create_prediction(object,opts) end
    members.reject=function(object) runtime._object_check(object); return runtime.reject() end
    members.close=function(object) return runtime._object_dispose(object) end
    members.write=function(object,values,opts)
        opts=copy(opts or {})
        for key in pairs(opts) do assert(key=='expected_revision' or key=='request_id' or key=='timeout_ms','unknown write option') end
        local _,status=runtime._write(object,values,opts.expected_revision,opts.request_id,opts.timeout_ms)
        assert(status==runtime.status.ok,'property write failed: '..tostring(status))
    end
    members.batch=function(object,fn,opts)
        assert(type(fn)=='function','batch callback required')
        opts=copy(opts or {})
        local draft=runtime._prediction_clone(object,true)
        local ok,err=pcall(runtime._prediction_run,draft,fn)
        if not ok then draft:close(); error(err,0) end
        local collected,values,revision=pcall(runtime._batch_values,object,draft)
        draft:close()
        if not collected then error(values,0) end
        assert(opts.expected_revision==nil or opts.expected_revision==revision,'batch revision override mismatch')
        opts.expected_revision=revision
        if next(values)~=nil then object:write(values,opts) end
    end
    for _,base in ipairs(chain) do record(base).sealed=true end
    data.flat=flat; data.members=members
    return flat
end
function Class:define()
    define(self)
    return self
end
local function construct(class,id,replica,subscription,values)
    local data=record(class); define(class)
    assert(type(id)=='string' and #id<=256, 'invalid entity ID')
    local object=runtime._object_create(data.name,replica,subscription,values)
    runtime._object_attach(object,data.members,id,data.name,{entity_id=id,entity_type=data.name,caller='client',subject='',peer=0})
    return object
end
function Class:spawn_or_restore(id,values)
    local data=record(self)
    assert(data.registered,'spawn requires a registered server class')
    assert(type(id)=='string' and #id>0 and #id<=256,'invalid entity ID')
    local _,status=runtime._spawn_entity(data.name,id,values or {})
    assert(status==runtime.status.ok,'entity spawn failed: '..tostring(status))
    return self:ref(id)
end
local function lifecycle_control(class,id,destroy)
    local data=record(class)
    assert(data.registered,'lifecycle requires a registered server class')
    local _,status=runtime._control_entity(data.name,id,destroy)
    assert(status==runtime.status.ok,'lifecycle operation failed: '..tostring(status))
end
function Class:transfer(id,target)
    local data=record(self)
    assert(data.registered,'transfer requires a registered server class')
    local _,status=runtime._transfer_entity(data.name,id,target)
    -- Return status so a timed-out handoff can be retried with the same target.
    return status
end
function Class:deactivate(id) return lifecycle_control(self,id,false) end
function Class:destroy(id) return lifecycle_control(self,id,true) end
function Class:spawn_local(values,opts)
    opts=copy(opts or {})
    for key in pairs(opts) do assert(key=='id','unknown local option') end
    return construct(self,opts.id or '',false,0,values or {})
end
function Class:watch(id,opts)
    local data=record(self); define(self)
    assert(type(id)=='string' and #id>0 and #id<=256,'invalid entity ID')
    opts=copy(opts or {})
    for key in pairs(opts) do assert(key=='timeout_ms','unknown watch option') end
    local bytes,status=runtime._watch(data.name,id,opts.timeout_ms)
    assert(status==runtime.status.ok,'watch failed: '..tostring(status))
    local subscription=string.unpack('>I8',bytes)
    local ok,object=pcall(construct,self,id,true,subscription)
    if not ok then runtime._unwatch(subscription); error(object,0) end
    return object
end
local function invoke(object, fn, payload)
    local result=table.pack(pcall(fn,object,payload))
    runtime._object_close(object)
    if not result[1] then error(result[2],0) end
    assert(result.n<=2 and (result[2]==nil or type(result[2])=='string'), 'entity RPC must return response bytes or nil')
    return result[2] or ''
end
function Class:register()
    local data=record(self)
    assert(not data.registered and data.name, 'entity class already registered or abstract root')
    local flat=define(self)
    local members=data.members
    local function attach(object,ctx)
        runtime._object_attach(object,members,ctx.entity_id,data.name,ctx)
        return object
    end
    local definition={attrs=flat.attrs,model=true,methods={},owner=flat.options.owner=='id' and 'id' or nil}
    for key,p in pairs(flat.rpcs) do
        local fn=members[key]
        definition.methods[key]={callers=p.callers,access=p.access,handler=function(object,ctx,payload)
            return invoke(attach(object,ctx),fn,payload)
        end}
    end
    definition.lifecycle={}
    for _,hook in ipairs({'create','activate'}) do
        local fn=members['on_'..hook]
        if fn then definition.lifecycle[hook]=function(object,ctx,payload) return invoke(attach(object,ctx),fn,payload) end end
    end
    definition.lifecycle.deactivate=function(object,ctx,reason)
        return invoke(attach(object,ctx),function(self)
            if members.on_deactivate then return members.on_deactivate(self,reason) end
        end,'')
    end
    definition.lifecycle.destroy=function(object,ctx,reason)
        return invoke(attach(object,ctx),function(self)
            if members.on_deactivate then members.on_deactivate(self,reason) end
            if members.on_destroy then return members.on_destroy(self,reason) end
        end,'')
    end
    if flat.methods.on_tick then
        definition.tick=function(object,ctx,step)
            return invoke(attach(object,ctx),members.on_tick,assert(tonumber(step),'invalid tick interval'))
        end
    end
    if flat.methods.validate_write then
        definition.validate_write=function(object,ctx,proposal)
            attach(object,ctx)
            local result=table.pack(pcall(members.validate_write,object,proposal))
            runtime._object_close(object)
            if not result[1] then error(result[2],0) end
            return result[2]==true
        end
    end
    runtime._object_registration(function() register_entity(data.name,definition) end)
    data.registered=true; data.exported=flat.rpcs
    return self
end
local function reference(kind,id,defaults,exported)
    name(kind)
    assert(type(id)=='string' and #id>0 and #id<=256, 'invalid entity ID')
    defaults=copy(defaults or {})
    for key in pairs(defaults) do assert(key=='peer' or key=='timeout_ms', 'invalid reference option') end
    local object=setmetatable({},ref_mt)
    refs[object]={type=kind,id=id,defaults=defaults,exported=exported}
    return object
end
function Class:ref(id,opts)
    local data=record(self); assert(data.name, 'use Entity:reference for an untyped reference')
    local exported=data.exported
    if not exported then
        local flat=flatten(self); exported=flat.rpcs
        if data.sealed then data.exported=exported end
    end
    return reference(data.name,id,opts,exported)
end
function Class:reference(kind,id,opts)
    record(self)
    return reference(kind,id,opts,nil)
end
local function call(ref,method,payload,opts)
    local data=assert(refs[ref],'expected an entity reference'); name(method)
    assert(not data.exported or data.exported[method], 'method is not an exported RPC: '..method)
    assert(type(payload)=='string', 'RPC payload must be bytes')
    opts=copy(opts or {})
    for key in pairs(opts) do assert(key=='timeout_ms' or key=='request_id', 'invalid RPC option') end
    local kind,id=runtime._current_entity()
    assert(kind~=data.type or id~=data.id, 'cannot await an RPC to the current entity; use a local method')
    local timeout=opts.timeout_ms or data.defaults.timeout_ms or 1000
    local request_id=opts.request_id
    if data.defaults.peer then return runtime.entity_request(data.defaults.peer,data.type,data.id,method,payload,timeout,request_id) end
    return runtime.entity_call(data.type,data.id,method,payload,timeout,request_id)
end
local visibility_actions={show_to=1,hide_from=2,possess=3,unpossess=4}
local function visibility(ref,peer,action)
    local data=assert(refs[ref],'expected an entity reference')
    local _,status=runtime._entity_visibility(peer,data.type,data.id,action)
    assert(status==runtime.status.ok,'entity visibility failed: '..tostring(status))
end
ref_mt.__index=function(ref,key)
    local data=assert(refs[ref],'invalid entity reference')
    if visibility_actions[key] then return function(object,peer) return visibility(object,peer,visibility_actions[key]) end end
    if key=='id' or key=='type' then return data[key] end
    if key=='call' then return call end
    if data.exported and data.exported[key] then
        return function(object,payload,opts) return call(object,key,payload,opts) end
    end
    error('unknown reference member: '..tostring(key),2)
end
ref_mt.__newindex=function() error('entity references are immutable',2) end
ref_mt.__metatable=false
class_mt.__index=function(class,key)
    if Class[key] then return Class[key] end
    local flat=record(class).flat or flatten(class)
    if flat.methods[key] then
        local fn=flat.methods[key]
        return function(object,...) check_domain(object,flat.domains[key]); return fn(object,...) end
    end
    error('unknown class member: '..tostring(key),2)
end
class_mt.__newindex=function(class,key,value)
    local data=editable(class); name(key)
    assert(not reserved[key] and not unsupported_hooks[key], 'reserved or unsupported lifecycle method')
    assert(type(value)=='function' and data.methods[key]==nil, 'methods must be functions and defined once')
    data.methods[key]=value
end
class_mt.__metatable=false
local Entity=setmetatable({},class_mt)
classes[Entity]={sealed=true,attrs={},methods={},rpcs={},domains={},options={}}
fsnet={Entity=Entity}
runtime.start=function(fn)
    assert(type(fn)=='function','start requires a function')
    runtime._start(function() fn(); return '' end)
end
runtime.sleep=function(ms)
    local _,status=runtime._sleep(ms)
    assert(status==runtime.status.ok,'sleep cancelled')
end

-- Input history owns only bounded immutable byte strings. All entity mutation is
-- performed on native candidates; a failed step/replay never replaces live state.
create_prediction=function(confirmed,opts)
    runtime._object_check(confirmed)
    assert(confirmed.mode=='replica','prediction requires a confirmed replica')
    opts=copy(opts or {})
    for key in pairs(opts) do assert(key=='max_inputs' or key=='max_bytes' or key=='acknowledged','unknown prediction option') end
    local max_inputs=opts.max_inputs or 128
    local max_bytes=opts.max_bytes or 262144
    assert(math.type(max_inputs)=='integer' and max_inputs>=1 and max_inputs<=128,'invalid prediction input limit')
    assert(math.type(max_bytes)=='integer' and max_bytes>=1 and max_bytes<=262144,'invalid prediction byte limit')
    local initial=opts.acknowledged or 0
    assert(math.type(initial)=='integer' and initial>=0,'invalid initial acknowledgement')
    local current={state=runtime._prediction_clone(confirmed),history={},bytes=0,
                   acked=initial,last=initial,revision=confirmed.revision}
    local closed=false
    local api={}
    local function check()
        assert(not closed,'prediction is closed')
        runtime._object_check(confirmed)
        runtime._object_check(current.state)
    end
    local function execute(candidate,input)
        local fn=candidate[input.method]
        assert(type(fn)=='function','prediction method required')
        return runtime._prediction_run(candidate,fn,input.payload)
    end
    function api:push(sequence,method,payload)
        check(); name(method)
        assert(not reserved[method] and method~='on_tick' and method~='validate_write','invalid prediction method')
        assert(math.type(sequence)=='integer' and sequence>current.last and sequence==current.last+1,'prediction sequence must be consecutive')
        assert(type(payload)=='string','prediction payload must be bytes')
        local charge=#method+#payload
        assert(#current.history<max_inputs and charge<=max_bytes-current.bytes,'prediction history limit')
        local input={sequence=sequence,method=method,payload=payload,charge=charge}
        local candidate=runtime._prediction_clone(current.state)
        local history={}
        for i,value in ipairs(current.history) do history[i]=value end
        history[#history+1]=input
        local next_state={state=candidate,history=history,bytes=current.bytes+charge,
                          acked=current.acked,last=sequence,revision=current.revision}
        local ok,result=pcall(execute,candidate,input)
        if not ok then candidate:close(); error(result,0) end
        -- Publish one reference: instruction/allocator failure cannot split the
        -- state and its history. Old read-only views are reclaimed by Lua GC.
        current=next_state
        -- Only first execution returns an effect description to the application.
        -- Reconciliation never dispatches these results.
        return result
    end
    function api:reconcile(sequence,confirmed_revision)
        check()
        assert(math.type(sequence)=='integer' and sequence>=current.acked and sequence<=current.last,'invalid acknowledgement sequence')
        assert(math.type(confirmed_revision)=='integer' and confirmed_revision>=current.revision and confirmed_revision==confirmed.revision,'confirmation does not match current snapshot')
        local candidate=runtime._prediction_clone(confirmed)
        local remaining,retained={},0
        local ok,err=pcall(function()
            for _,input in ipairs(current.history) do
                if input.sequence>sequence then
                    execute(candidate,input)
                    remaining[#remaining+1]=input; retained=retained+input.charge
                end
            end
        end)
        if not ok then candidate:close(); error(err,0) end
        current={state=candidate,history=remaining,bytes=retained,acked=sequence,
                 last=current.last,revision=confirmed_revision}
    end
    function api:close()
        if not closed then current.state:close(); current.history={}; current.bytes=0; closed=true end
    end
    return setmetatable({}, {
        __index=function(_,key)
            if key=='close' then return api.close end
            check()
            if key=='state' then return current.state end
            if key=='confirmed' then return confirmed end
            if key=='pending' then return #current.history end
            if key=='acknowledged' then return current.acked end
            if key=='revision' then return current.revision end
            return assert(api[key],'unknown prediction member')
        end,
        __newindex=function() error('prediction controllers are immutable',2) end,
        __metatable=false,
    })
end

-- Printing and ambient RNG state are external effects too. Preserve normal use
-- outside prediction and reject even captured references inside a reducer.
local function guard_effect(fn)
    return function(...) runtime._prediction_guard(); return fn(...) end
end
print=guard_effect(print)
warn=guard_effect(warn)
math.random=guard_effect(math.random)
math.randomseed=guard_effect(math.randomseed)

-- Presentation factories wrap native confirmed replicas. The server owns the
-- entity type; the client chooses local/remote behaviour from trusted metadata.
local entities={}
fsnet.entities=entities
local factories,views={},{ }
local started,running=false,false
local function view_key(kind,id) return kind..'\0'..id end
local function lifecycle(view,hook)
    local fn=view[hook]
    if fn then assert(type(fn)=='function','invalid presentation hook'); fn(view) end
end
local function control(view,value)
    if view.controlled==value then return end
    view.controlled=value
    lifecycle(view,value and 'on_possess' or 'on_unpossess')
end
local function remove_view(key)
    local view=views[key]
    if not view then return end
    views[key]=nil
    -- Cleanup must still release native resources if a presentation hook fails.
    local ok,err=pcall(control,view,false)
    local disposed,dispose_err=pcall(lifecycle,view,'on_despawn')
    view.entity:close()
    if not ok then error(err,0) end
    if not disposed then error(dispose_err,0) end
end
function entities:register(class,definition)
    assert(self==entities and not started,'register presentations before start')
    local data=record(class); define(class)
    assert(not factories[data.name] and type(definition)=='table','invalid or duplicate presentation')
    for key in pairs(definition) do assert(key=='owned' or key=='observed','unknown presentation option') end
    assert(type(definition.owned)=='table' and type(definition.observed)=='table','local/remote prototypes required')
    factories[data.name]={class=class,owned=definition.owned,observed=definition.observed}
end
function entities:get(kind,id)
    local view=views[view_key(kind,id)]
    if view and pcall(runtime._object_check,view.entity) then return view end
    return nil -- revocation is immediate, before the next lifecycle dispatch
end
function entities:stop() running=false end
function entities:start()
    assert(self==entities and not started,'entity presentations already started')
    started=true; running=true
    runtime.start(function()
        local function dispatch(event)
            local key=view_key(event.type,event.id)
            local view=views[key]
            if event.action==2 then
                if view and view.subscription==event.subscription then remove_view(key) end
            elseif event.action==3 then
                if view and view.subscription==event.subscription then
                    if event.controlled and not pcall(runtime._object_check,view.entity) then remove_view(key)
                    else control(view,event.controlled) end
                end
            else
                if view and view.subscription==event.subscription then return end
                if view then remove_view(key) end
                local factory=assert(factories[event.type],'no presentation factory for '..event.type)
                -- A queued enter can already have been revoked before Lua polls.
                local ok,entity=pcall(construct,factory.class,event.id,true,event.subscription)
                if not ok then
                    local alive=runtime._subscription_alive(event.subscription)
                    runtime._unwatch(event.subscription)
                    if alive then error(entity,0) end
                    return
                end
                view=setmetatable({entity=entity,id=event.id,type=event.type,owned=event.owned,
                                   controlled=false,subscription=event.subscription},
                                  {__index=event.owned and factory.owned or factory.observed})
                views[key]=view
                lifecycle(view,'on_spawn')
                control(view,event.controlled)
            end
        end
        local ok,err=pcall(function()
            while running do
                local event=runtime._entity_event()
                if event then dispatch(event) else runtime.sleep(16) end
            end
        end)
        running=false
        local keys={}
        for key in pairs(views) do keys[#keys+1]=key end
        for _,key in ipairs(keys) do pcall(remove_view,key) end
        runtime._stop_entity_events()
        if not ok then error(err,0) end
    end)
end

-- The client host resolves peer 0 to its configured authenticated owner endpoint.
runtime.server_request=function(route,payload,timeout)
    return runtime.request(0,route,payload,timeout)
end

-- Structural value types. Definitions are copied into each sealed class schema.
fsnet.struct=function(kind,fields)
    name(kind)
    assert(type(fields)=='table' and next(fields)~=nil,'struct requires fields')
    local count=0
    for key,value in pairs(fields) do
        name(key); count=count+1
        assert(count<=64 and (type(value)=='string' or type(value)=='table'),'invalid struct field type')
    end
    return {kind='struct',name=kind,fields=copy(fields)}
end
fsnet.array=function(element,length)
    assert((type(element)=='string' or type(element)=='table') and math.type(length)=='integer' and length>=1 and length<=256,'invalid fixed array type')
    return {kind='array',element=copy(element),length=length}
end
-- Variable containers have a declared element type and bounded capacity.
local function container(kind,element,max_items)
    if max_items==nil then max_items=256 end
    assert((type(element)=='string' or type(element)=='table') and math.type(max_items)=='integer' and max_items>=1 and max_items<=256,'invalid container type')
    return {kind=kind,element=copy(element),max_items=max_items}
end
fsnet.list=function(element,max_items) return container('list',element,max_items) end
fsnet.map=function(element,max_items) return container('map',element,max_items) end
fsnet.set=function(element,max_items) return container('set',element,max_items) end
fsnet.vec3=fsnet.struct('Vec3',{x='number',y='number',z='number'})
fsnet.mat4x4=fsnet.array(fsnet.array('number',4),4)

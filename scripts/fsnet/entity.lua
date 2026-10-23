-- Built into the Lua adapter. No filesystem/package loader is required at runtime.
local classes = setmetatable({}, {__mode='k'})
local names = setmetatable({}, {__mode='v'})
local refs = setmetatable({}, {__mode='k'})
local Class, class_mt, ref_mt = {}, {}, {}
local register_entity=runtime.entity
local reserved = {id=true, type=true, context=true, reject=true, attr=true, rpc=true,
                  private=true, extend=true, register=true, ref=true, reference=true,
                  define=true, spawn_local=true, watch=true, write=true, close=true,
                  revision=true, mode=true, method=true}
local unsupported_hooks = {new=true, init=true, on_create=true, on_activate=true, on_deactivate=true}
local function name(value)
    assert(type(value)=='string' and #value>0 and #value<=127 and not value:find('[%.%z]') and value:sub(1,1)~='$', 'invalid entity/member name')
    return value
end
local function copy(value, seen, depth)
    if type(value)~='table' then return value end
    seen=seen or {}; depth=depth or 0
    assert(depth<8 and not seen[value], 'recursive or excessively nested definition')
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
    assert(not reserved[key] and not unsupported_hooks[key] and key~='on_tick' and key~='validate_write', 'reserved property name')
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
    assert(not reserved[key] and key~='on_tick' and key~='validate_write' and not unsupported_hooks[key], 'reserved RPC name')
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
    members.reject=function(object) runtime._object_check(object); return runtime.reject() end
    members.close=function(object) return runtime._object_dispose(object) end
    members.write=function(object,values,opts)
        opts=copy(opts or {})
        for key in pairs(opts) do assert(key=='expected_revision' or key=='request_id' or key=='timeout_ms','unknown write option') end
        local _,status=runtime._write(object,values,opts.expected_revision,opts.request_id,opts.timeout_ms)
        assert(status==runtime.status.ok,'property write failed: '..tostring(status))
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
ref_mt.__index=function(ref,key)
    local data=assert(refs[ref],'invalid entity reference')
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

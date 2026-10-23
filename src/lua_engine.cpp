#include "fsnet/lua_engine.hpp"
#include "fsnet/attribute_wire.hpp"
#include "fsnet_lua_object_api.hpp"
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include <algorithm>
#include <cstdlib>
#include <cmath>
#include <deque>
#include <limits>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace fsnet {
struct LuaEngine::Impl : std::enable_shared_from_this<LuaEngine::Impl> {
    using Clock = std::chrono::steady_clock;
    struct Await {
        std::uint64_t peer;
        std::string route, data;
        std::chrono::milliseconds timeout;
        enum class Kind { rpc, sleep, watch, write, spawn, visibility } kind = Kind::rpc;
        std::uint64_t subscription = 0;
        AttributeWriteOptions options;
        EntityAttributes::Values values;
        unsigned action=0;
    };
    struct Task {
        lua_State* thread = nullptr;
        int reference = LUA_NOREF, function = LUA_NOREF;
        std::string data, entity_id, entity_type, mailbox;
        Reply reply;
        CallIdentity identity;
        bool contextual = false, rejected = false;
        std::optional<Await> await;
        bool waiting = false, startup = false, background = false, completion_queued = false;
        std::optional<Clock::time_point> wake;
        std::uint64_t generation = 0;
        Clock::time_point deadline{};
    };
    struct Resume { std::uint64_t id; Result result; };
    std::shared_ptr<ScriptHost> host;
    ScriptLimits limits;
    lua_State* state = nullptr;
    std::size_t used = 0;
    std::size_t queued_bytes = 0;
    std::size_t completed_bytes = 0;
    bool stopped = false, loaded = false, loading = false, polling = false, stop_requested = false;
    bool exhausted = false;
    unsigned budget = 0;
    std::uint64_t serial = 0;
    std::unordered_map<std::string, int> routes;
    std::unordered_map<std::string, std::string> entity_routes, stateful_routes;
    std::unordered_map<std::string, RpcPolicy> policies;
    std::unordered_set<std::string> busy;
    std::vector<std::string> bound;
    std::deque<Task> queue;
    std::unordered_map<std::uint64_t, Task> tasks;
    std::unordered_map<lua_State*, std::uint64_t> threads;
    std::deque<Resume> resumes;
    std::string error, chunk_name;
    std::string_view source;
    Task* preparing = nullptr;
    Result* returning = nullptr;
    int compiled = LUA_NOREF;
    std::vector<int> starts;

    static Impl& self(lua_State* L) { return **static_cast<Impl**>(lua_getextraspace(L)); }
    static void* allocate(void* opaque, void* pointer, std::size_t old_size, std::size_t new_size) {
        auto& owner = *static_cast<Impl*>(opaque);
        const auto old = pointer ? old_size : 0;
        if (!new_size) { std::free(pointer); owner.used -= old; return nullptr; }
        const auto retained = owner.used - old;
        if (new_size > owner.limits.memory_bytes - retained) return nullptr;
        auto* next = std::realloc(pointer, new_size);
        if (next) owner.used = retained + new_size;
        return next;
    }
    enum class ObjectMode { request, local, replica, prediction };
    struct AttributeObject {
        Impl* engine; std::size_t charge; EntityAttributes model; std::uint64_t task = 0;
        ObjectMode mode = ObjectMode::request;
        std::uint64_t subscription = 0;
        std::unordered_set<std::string> present;
        std::string kind;
        std::shared_ptr<const AttributeUpdate> projection;
        std::size_t fixed_charge = 0;
    };
    AttributeObject* predicting = nullptr;
    bool prediction_violation = false;
    static void prediction_external(lua_State* L) {
        auto& engine = self(L);
        if (engine.predicting) {
            engine.prediction_violation = true;
            luaL_error(L, "external effects and waits are forbidden during prediction/replay");
        }
    }
    std::unordered_map<std::string, EntitySchema> schemas;
    std::unordered_set<std::string> server_schemas;
    std::vector<std::string> defined_schemas;
    static std::string text_field(lua_State* L, int table, const char* field, const char* fallback) {
        lua_getfield(L, table, field);
        std::size_t n = 0; const char* s = lua_isnil(L, -1) ? fallback : luaL_checklstring(L, -1, &n);
        std::string value = lua_isnil(L, -1) ? std::string(fallback) : std::string(s, n);
        lua_pop(L, 1); return value;
    }
    static Access attribute_access(const std::string& s) {
        if (s == "none") return Access::none;
        if (s == "r") return Access::read;
        if (s == "w") return Access::write;
        if (s == "rw") return Access::read_write;
        throw std::invalid_argument("invalid attribute access");
    }
    static std::size_t value_nodes(const AttributeValue& value) {
        std::size_t n=1;
        if(auto fields=std::get_if<AttributeStruct>(&value)) for(const auto& [key,v]:*fields) { (void)key; n+=value_nodes(v); }
        if(auto items=std::get_if<AttributeArray>(&value)) for(const auto& v:*items) n+=value_nodes(v);
        if(auto entries=std::get_if<AttributeMap>(&value)) for(const auto& [key,v]:*entries) { (void)key; n+=value_nodes(v); }
        return n;
    }
    static std::size_t carrier_charge(const EntityAttributes& model) {
        auto schema_bytes=[&](auto&& recurse,const AttributeDefinition& d)->std::size_t {
            std::size_t n=512;
            AttributeStruct initial{{"value",d.initial}}; n+=attribute_image_bytes(initial);
            for(const auto& [key,child]:d.fields) n+=key.size()+recurse(recurse,child);
            if(d.element) n+=recurse(recurse,*d.element);
            return n;
        };
        std::size_t charge=65536+4*attribute_image_bytes(model.all_values());
        for(const auto& [key,d]:model.schema().attributes) charge+=key.size()+2*schema_bytes(schema_bytes,d);
        return charge;
    }
    static AttributeDefinition value_definition(lua_State* L,int index,unsigned depth=0,std::size_t* remaining=nullptr) {
        std::size_t count=4096; if(!remaining) remaining=&count;
        if(depth>8 || !*remaining) throw std::length_error("struct type depth/node limit");
        --*remaining; index=lua_absindex(L,index);
        AttributeDefinition d{AttributeType::integer,std::int64_t(0)};
        if(lua_type(L,index)==LUA_TSTRING) {
            std::string type=lua_tostring(L,index);
            if(type=="int32") { d.minimum=INT32_MIN; d.maximum=INT32_MAX; }
            else if(type=="uint32") { d.minimum=0; d.maximum=UINT32_MAX; }
            else if(type=="int64") {}
            else if(type=="number" || type=="double") { d.type=AttributeType::number; d.initial=0.0; }
            else if(type=="bool" || type=="boolean") { d.type=AttributeType::boolean; d.initial=false; }
            else if(type=="string") { d.type=AttributeType::string; d.initial=std::string{}; }
            else if(type=="vec3") {
                d.type=AttributeType::structure;
                for(auto key:{"x","y","z"}) d.fields.emplace(key,AttributeDefinition{AttributeType::number,0.0});
                d.initial=AttributeStruct{{"x",0.0},{"y",0.0},{"z",0.0}};
            } else if(type=="mat4x4") {
                AttributeDefinition number{AttributeType::number,0.0};
                AttributeDefinition row{AttributeType::array,AttributeArray(4,0.0)};
                row.element=std::make_shared<const AttributeDefinition>(std::move(number)); row.length=4;
                d.type=AttributeType::array; d.length=4; d.initial=AttributeArray(4,row.initial);
                d.element=std::make_shared<const AttributeDefinition>(std::move(row));
            } else throw std::invalid_argument("unsupported attribute type");
            return d;
        }
        if(lua_type(L,index)!=LUA_TTABLE) throw std::invalid_argument("attribute type must be a scalar name or descriptor");
        auto kind=text_field(L,index,"kind","");
        lua_pushnil(L);
        while(lua_next(L,index)) {
            if(lua_type(L,-2)!=LUA_TSTRING) throw std::invalid_argument("named type descriptor options required");
            std::string key=lua_tostring(L,-2);
            if(key!="kind" && !(kind=="struct" && (key=="name" || key=="fields")) &&
               !((kind=="array" || kind=="list" || kind=="map" || kind=="set") && (key=="element" || (kind=="array" ? key=="length" : key=="max_items")))) throw std::invalid_argument("unknown type descriptor option");
            lua_pop(L,1);
        }
        if(kind=="struct") {
            d.type=AttributeType::structure; AttributeStruct initial;
            lua_getfield(L,index,"fields");
            if(lua_type(L,-1)!=LUA_TTABLE) throw std::invalid_argument("struct fields required");
            auto fields=lua_absindex(L,-1); lua_pushnil(L);
            while(lua_next(L,fields)) {
                if(lua_type(L,-2)!=LUA_TSTRING || d.fields.size()>=64) throw std::invalid_argument("named struct fields required, maximum 64");
                std::size_t n; const char* key=lua_tolstring(L,-2,&n); std::string name(key,n);
                auto child=value_definition(L,-1,depth+1,remaining);
                if(value_nodes(AttributeValue(initial))+value_nodes(child.initial)>4096) throw std::length_error("struct expanded node limit");
                initial.emplace(name,child.initial); d.fields.emplace(std::move(name),std::move(child)); lua_pop(L,1);
            }
            lua_pop(L,1); d.initial=std::move(initial);
        } else if(kind=="array") {
            lua_getfield(L,index,"length"); auto length=luaL_checkinteger(L,-1); lua_pop(L,1);
            if(length<1 || length>256) throw std::invalid_argument("fixed array length must be 1..256");
            lua_getfield(L,index,"element"); auto element=value_definition(L,-1,depth+1,remaining); lua_pop(L,1);
            if(static_cast<std::size_t>(length)>(4096-1)/value_nodes(element.initial)) throw std::length_error("array expanded node limit");
            d.type=AttributeType::array; d.length=static_cast<std::size_t>(length);
            d.initial=AttributeArray(d.length,element.initial);
            d.element=std::make_shared<const AttributeDefinition>(std::move(element));
        } else if(kind=="list" || kind=="map" || kind=="set") {
            lua_getfield(L,index,"max_items");
            if(!lua_isnil(L,-1) && !lua_isinteger(L,-1)) throw std::invalid_argument("integer container capacity required");
            auto capacity=lua_isnil(L,-1)?256:lua_tointeger(L,-1); lua_pop(L,1);
            if(capacity<1 || capacity>256) throw std::invalid_argument("container capacity must be 1..256");
            lua_getfield(L,index,"element"); auto element=value_definition(L,-1,depth+1,remaining); lua_pop(L,1);
            d.type=kind=="list"?AttributeType::list:kind=="map"?AttributeType::map:AttributeType::set;
            d.max_items=static_cast<std::size_t>(capacity);
            d.initial=kind=="map"?AttributeValue(AttributeMap{}):AttributeValue(AttributeArray{});
            d.element=std::make_shared<const AttributeDefinition>(std::move(element));
        } else throw std::invalid_argument("invalid structured type descriptor");
        return d;
    }
    static AttributeValue attribute_value(lua_State* L,int index,const AttributeDefinition& d,unsigned depth=0,std::size_t* remaining=nullptr) {
        std::size_t budget=4096; if(!remaining) remaining=&budget;
        if(!*remaining) throw std::length_error("attribute value node limit");
        --*remaining;
        if(depth>8) throw std::length_error("attribute value depth limit");
        index=lua_absindex(L,index);
        if(luaL_testudata(L,index,"fsnet.field")) {
            auto value=proxy_value(L,index); auto nodes=value_nodes(value)-1;
            if(nodes>*remaining) throw std::length_error("attribute value node limit");
            *remaining-=nodes; return value;
        }
        if(d.type==AttributeType::integer) {
            if(!lua_isinteger(L,index)) throw std::invalid_argument("integer attribute required");
            return static_cast<std::int64_t>(lua_tointeger(L,index));
        }
        if(d.type==AttributeType::number) {
            if(lua_type(L,index)!=LUA_TNUMBER || !std::isfinite(lua_tonumber(L,index))) throw std::invalid_argument("finite number attribute required");
            return static_cast<double>(lua_tonumber(L,index));
        }
        if(d.type==AttributeType::boolean) {
            if(!lua_isboolean(L,index)) throw std::invalid_argument("boolean attribute required");
            return bool(lua_toboolean(L,index));
        }
        if(d.type==AttributeType::string) {
            if(lua_type(L,index)!=LUA_TSTRING) throw std::invalid_argument("string attribute required");
            std::size_t n; const char* text=lua_tolstring(L,index,&n);
            if(n>65536) throw std::length_error("attribute string limit");
            return std::string(text,n);
        }
        if(lua_type(L,index)!=LUA_TTABLE) throw std::invalid_argument("structured attribute requires a table or field view");
        if(lua_getmetatable(L,index)) { lua_pop(L,1); throw std::invalid_argument("attribute values must use plain tables"); }
        if(d.type==AttributeType::map) {
            AttributeMap entries; lua_pushnil(L);
            while(lua_next(L,index)) {
                if(lua_type(L,-2)!=LUA_TSTRING || entries.size()>=d.max_items) throw std::invalid_argument("map requires string keys within capacity");
                std::size_t n; const char* key=lua_tolstring(L,-2,&n);
                if(!n || n>127 || std::string_view(key,n).find('\0')!=std::string_view::npos) throw std::invalid_argument("invalid map key");
                entries.emplace(std::string(key,n),attribute_value(L,-1,*d.element,depth+1,remaining)); lua_pop(L,1);
            }
            return entries;
        }
        std::size_t count=0; lua_pushnil(L);
        while(lua_next(L,index)) {
            if(d.type==AttributeType::structure) {
                if(lua_type(L,-2)!=LUA_TSTRING) throw std::invalid_argument("struct requires named fields");
                std::size_t n; const char* text=lua_tolstring(L,-2,&n);
                if(!d.fields.contains(std::string(text,n))) throw std::invalid_argument("unknown struct field");
            } else if(!lua_isinteger(L,-2) || lua_tointeger(L,-2)<1 || static_cast<std::uint64_t>(lua_tointeger(L,-2))>(d.type==AttributeType::array?d.length:d.max_items))
                throw std::invalid_argument("fixed array requires dense integer indices");
            if(++count>256) throw std::length_error("attribute entry limit");
            lua_pop(L,1);
        }
        if(d.type==AttributeType::structure) {
            if(count!=d.fields.size()) throw std::invalid_argument("missing struct field");
            AttributeStruct value;
            for(const auto& [key,child]:d.fields) {
                lua_getfield(L,index,key.c_str()); value.emplace(key,attribute_value(L,-1,child,depth+1,remaining)); lua_pop(L,1);
            }
            return value;
        }
        if(d.type==AttributeType::array && count!=d.length) throw std::invalid_argument("fixed array length mismatch");
        AttributeArray value; value.reserve(count);
        for(std::size_t i=1;i<=count;++i) {
            lua_rawgeti(L,index,static_cast<lua_Integer>(i)); auto child=attribute_value(L,-1,*d.element,depth+1,remaining); lua_pop(L,1);
            if(d.type==AttributeType::set && std::find(value.begin(),value.end(),child)!=value.end()) throw std::invalid_argument("duplicate set element");
            value.push_back(std::move(child));
        }
        return value;
    }
    static const AttributeStruct* value_members(const AttributeValue& value) {
        if(auto fields=std::get_if<AttributeStruct>(&value)) return fields;
        return std::get_if<AttributeMap>(&value);
    }
    static AttributeStruct* value_members(AttributeValue& value) {
        if(auto fields=std::get_if<AttributeStruct>(&value)) return fields;
        return std::get_if<AttributeMap>(&value);
    }
    static void push_attribute(lua_State* L,const AttributeValue& value) {
        if(auto n=std::get_if<std::int64_t>(&value)) lua_pushinteger(L,static_cast<lua_Integer>(*n));
        else if(auto n=std::get_if<double>(&value)) lua_pushnumber(L,*n);
        else if(auto b=std::get_if<bool>(&value)) lua_pushboolean(L,*b);
        else if(auto text=std::get_if<std::string>(&value)) lua_pushlstring(L,text->data(),text->size());
        else if(auto fields=value_members(value)) {
            lua_createtable(L,0,static_cast<int>(fields->size()));
            for(const auto& [key,child]:*fields) { push_attribute(L,child); lua_setfield(L,-2,key.c_str()); }
        } else {
            const auto& items=std::get<AttributeArray>(value); lua_createtable(L,static_cast<int>(items.size()),0);
            for(std::size_t i=0;i<items.size();++i) { push_attribute(L,items[i]); lua_rawseti(L,-2,static_cast<lua_Integer>(i+1)); }
        }
    }
    static int attributes_define(lua_State* L) {
        auto& engine = self(L);
        if (!engine.loading) return luaL_error(L, "declare attributes only during load");
        std::size_t n; const char* type = luaL_checklstring(L, 1, &n);
        luaL_checktype(L, 2, LUA_TTABLE);
        try {
            std::string kind(type, n);
            if (kind.empty() || kind.size() > 127 || kind.find_first_of(".\0", 0, 2) != std::string::npos || engine.schemas.size() >= 256)
                throw std::invalid_argument("invalid attribute entity kind");
            EntitySchema schema;
            if (!lua_isnoneornil(L,3)) {
                std::size_t length; const char* owner = luaL_checklstring(L,3,&length);
                if (std::string_view(owner,length)!="id") throw std::invalid_argument("unsupported declarative entity owner");
                schema.owner_from_id = true;
            }
            lua_pushnil(L);
            while (lua_next(L, 2)) {
                if (schema.attributes.size() >= 256) throw std::length_error("attribute count limit");
                if (lua_type(L, -2) != LUA_TSTRING || lua_type(L, -1) != LUA_TTABLE) throw std::invalid_argument("named attribute definitions required");
                std::size_t length; const char* key = lua_tolstring(L, -2, &length); std::string name(key, length);
                int definition = lua_gettop(L);
                lua_getfield(L,definition,"type"); auto d=value_definition(L,-1); lua_pop(L,1);
                d.server = attribute_access(text_field(L, definition, "server", "rw"));
                d.client = attribute_access(text_field(L, definition, "client", "r"));
                auto visibility = text_field(L, definition, "sync", (d.client == Access::none || d.client == Access::write) ? "none" : "owner");
                if (visibility == "owner") d.visibility = Visibility::owner;
                else if (visibility == "observers") d.visibility = Visibility::observers;
                else if (visibility == "none") d.visibility = Visibility::none;
                else throw std::invalid_argument("invalid property synchronization scope");
                lua_getfield(L, definition, "persist");
                if (!lua_isnil(L, -1) && !lua_isboolean(L, -1)) throw std::invalid_argument("persist must be boolean");
                d.persist = lua_isnil(L, -1) || lua_toboolean(L, -1); lua_pop(L, 1);
                auto authority=text_field(L,definition,"authority","server");
                if(authority!="driver" && authority!="server") throw std::invalid_argument("invalid property authority");
                d.driver_authority=authority=="driver";
                lua_getfield(L, definition, "default");
                if(!lua_isnil(L,-1) || static_cast<unsigned>(d.type)<4) d.initial=attribute_value(L,-1,d);
                lua_pop(L,1);
                schema.attributes.emplace(std::move(name), std::move(d)); lua_pop(L, 1);
            }
            schema.validate(); EntityAttributes model(schema, "");
            auto full = encode_attributes({0, 0, true, model.all_values()}); (void)full;
            auto initial = encode_attributes({0, 0, true, model.persistent()});
            if (!engine.schemas.emplace(kind, std::move(schema)).second) throw std::invalid_argument("duplicate entity attributes");
            if(!lua_toboolean(L,4)) engine.server_schemas.insert(kind);
            lua_pushlstring(L, initial.data(), initial.size()); return 1;
        } catch (const std::exception& e) { return luaL_error(L, "%s", e.what()); }
    }
    static AttributeObject& attribute_object(lua_State* L, int index) {
        return *static_cast<AttributeObject*>(luaL_checkudata(L, index, "fsnet.attributes"));
    }
    static int attributes_open(lua_State* L) {
        prediction_external(L);
        auto& engine = self(L);
        std::size_t n, size; const char* kind = luaL_checklstring(L, 1, &n); const char* state = luaL_checklstring(L, 2, &size);
        try {
            const auto& schema = engine.schemas.at(std::string(kind, n));
            auto image = decode_attributes(std::string_view(state, size));
            if (!image.snapshot) throw std::invalid_argument("attribute state must be a snapshot");
            std::string account;
            if (engine.threads.contains(L)) account = engine.host->entity_owner(std::string(kind,n), engine.tasks.at(engine.threads.at(L)).entity_id);
            EntityAttributes model(schema, account); model.restore_full(image.values, image.revision);
            auto charge = carrier_charge(model);
            if (charge > engine.limits.memory_bytes - engine.used) throw std::length_error("attribute object memory limit");
            void* memory = lua_newuserdatauv(L, sizeof(AttributeObject), 1);
            if (charge > engine.limits.memory_bytes - engine.used) throw std::length_error("attribute object memory limit");
            auto* object=new(memory) AttributeObject{&engine, charge, std::move(model), engine.threads.contains(L) ? engine.threads.at(L) : 0,ObjectMode::request,0,{},std::string(kind,n),{}};
            object->fixed_charge=charge-4*attribute_image_bytes(object->model.all_values());
            engine.used += charge; luaL_setmetatable(L, "fsnet.attributes"); return 1;
        } catch (const std::exception& e) { return luaL_error(L, "%s", e.what()); }
    }
    static int attributes_gc(lua_State* L) {
        auto& object = attribute_object(L, 1); object.engine->used -= object.charge;
        if(object.subscription) try { object.engine->host->unwatch_attributes(object.subscription); } catch(...) {}
        object.~AttributeObject(); return 0;
    }
    static void refresh(AttributeObject& object) {
        if(object.mode!=ObjectMode::replica || !object.subscription) return;
        auto image=object.engine->host->attribute_snapshot(object.subscription);
        if(!image) throw std::logic_error("attribute subscription was revoked");
        if(image==object.projection) return;
        EntityAttributes next(object.model.schema(),""); next.restore_full(image->values,image->revision);
        (void)attribute_wire_size(*image);
        auto charge=object.fixed_charge+4*attribute_image_bytes(next.all_values());
        if(charge>object.charge && charge-object.charge>object.engine->limits.memory_bytes-object.engine->used) throw std::length_error("replica memory budget");
        std::unordered_set<std::string> present;
        for(const auto& [key,value]:image->values) { (void)value; present.insert(key); }
        object.engine->used=object.engine->used-object.charge+charge;
        object.charge=charge; object.model=std::move(next); object.present=std::move(present);
        object.projection=std::move(image);
    }
    static void object_check(lua_State* L, int index, bool require_attached = false) {
        auto& object = attribute_object(L,index);
        lua_getiuservalue(L,index,1);
        if (lua_isnil(L,-1) && require_attached) { lua_pop(L,1); throw std::logic_error("expected an attached entity object"); }
        if (!lua_isnil(L,-1)) {
            lua_getfield(L,-1,"active"); bool active = lua_toboolean(L,-1); lua_pop(L,1);
            auto& engine = *object.engine;
            if (!active || (object.mode==ObjectMode::request && (!engine.threads.contains(L) || engine.threads.at(L)!=object.task))) {
                lua_pop(L,1); throw std::logic_error("entity object is outside its request scope");
            }
        }
        if (object.mode==ObjectMode::prediction) {
            lua_getfield(L,-1,"confirmed");
            object_check(L,-1,true);
            lua_pop(L,1);
        }
        lua_pop(L,1);
        refresh(object);
    }
    static int object_registration(lua_State* L) {
        auto& engine=self(L);
        if (!engine.loading) return luaL_error(L,"register classes only during load");
        luaL_checktype(L,1,LUA_TFUNCTION);
        try {
            std::unordered_set<std::string> routes, schemas;
            auto published=engine.server_schemas;
            for (const auto& [route,reference] : engine.routes) { (void)reference; routes.insert(route); }
            for (const auto& [type,schema] : engine.schemas) { (void)schema; schemas.insert(type); }
            lua_pushvalue(L,1);
            int status=lua_pcall(L,0,0,0);
            if (status!=LUA_OK) {
                engine.server_schemas=std::move(published);
                for (auto it=engine.routes.begin(); it!=engine.routes.end();) {
                    if (routes.contains(it->first)) { ++it; continue; }
                    luaL_unref(L,LUA_REGISTRYINDEX,it->second);
                    engine.entity_routes.erase(it->first); engine.stateful_routes.erase(it->first); engine.policies.erase(it->first);
                    it=engine.routes.erase(it);
                }
                for (auto it=engine.schemas.begin(); it!=engine.schemas.end();) {
                    if (schemas.contains(it->first)) ++it; else { engine.server_schemas.erase(it->first); it=engine.schemas.erase(it); }
                }
                return lua_error(L);
            }
            return 0;
        } catch (const std::exception& e) { return luaL_error(L,"%s",e.what()); }
    }
    static int object_validate(lua_State* L) {
        try { object_check(L,1,true); return 0; }
        catch (const std::exception& e) { return luaL_error(L,"%s",e.what()); }
    }
    static int object_attach(lua_State* L) {
        prediction_external(L);
        auto& object = attribute_object(L,1); luaL_checktype(L,2,LUA_TTABLE);
        std::size_t id_size,type_size; const char* id=luaL_checklstring(L,3,&id_size); const char* type=luaL_checklstring(L,4,&type_size);
        luaL_checktype(L,5,LUA_TTABLE);
        lua_getiuservalue(L,1,1); bool attached=!lua_isnil(L,-1); lua_pop(L,1);
        auto& engine=*object.engine;
        if(attached || object.kind!=std::string_view(type,type_size)) return luaL_error(L,"invalid entity object attachment");
        if(object.mode==ObjectMode::request) {
            if(!engine.threads.contains(L) || object.task!=engine.threads.at(L)) return luaL_error(L,"invalid entity object attachment");
            const auto& task=engine.tasks.at(object.task);
            if(std::string_view(id,id_size)!=task.entity_id || std::string_view(type,type_size)!=task.entity_type)
                return luaL_error(L,"entity object identity mismatch");
        }
        lua_createtable(L,0,5);
        lua_pushvalue(L,2); lua_setfield(L,-2,"members");
        lua_pushvalue(L,3); lua_setfield(L,-2,"id"); lua_pushvalue(L,4); lua_setfield(L,-2,"type");
        lua_pushvalue(L,5); lua_setfield(L,-2,"context"); lua_pushboolean(L,true); lua_setfield(L,-2,"active");
        lua_setiuservalue(L,1,1); return 0;
    }
    static int object_close(lua_State* L) {
        prediction_external(L);
        try { object_check(L,1,true); }
        catch (const std::exception& e) { return luaL_error(L,"%s",e.what()); }
        lua_getiuservalue(L,1,1);
        if (!lua_istable(L,-1)) return luaL_error(L,"entity object is not attached");
        lua_pushboolean(L,false); lua_setfield(L,-2,"active"); lua_pop(L,1); return 0;
    }
    static int current_entity(lua_State* L) {
        auto& engine=self(L);
        if (!engine.threads.contains(L)) { lua_pushnil(L); lua_pushnil(L); return 2; }
        const auto& task=engine.tasks.at(engine.threads.at(L));
        lua_pushlstring(L,task.entity_type.data(),task.entity_type.size());
        lua_pushlstring(L,task.entity_id.data(),task.entity_id.size()); return 2;
    }
    static int attributes_index(lua_State* L) {
        std::size_t n; const char* name=luaL_checklstring(L,2,&n);
        try {
            // Revocation must not prevent explicit resource cleanup.
            if(std::string_view(name,n)=="close" && attribute_object(L,1).mode!=ObjectMode::request) {
                lua_pushcfunction(L,object_dispose); return 1;
            }
            object_check(L,1);
            auto& object=attribute_object(L,1);
            auto key=std::string(name,n);
            if(key=="revision") { lua_pushinteger(L,static_cast<lua_Integer>(object.model.revision())); return 1; }
            if(key=="mode") { lua_pushstring(L,object.mode==ObjectMode::request?"server":object.mode==ObjectMode::local?"local":object.mode==ObjectMode::prediction?"prediction":"replica"); return 1; }
            lua_getiuservalue(L,1,1);
            if (lua_istable(L,-1)) {
                auto key=std::string_view(name,n);
                if (key=="id" || key=="type" || key=="context") { lua_pushvalue(L,2); lua_gettable(L,-2); return 1; }
                lua_getfield(L,-1,"members"); lua_pushvalue(L,2); lua_gettable(L,-2);
                if (!lua_isnil(L,-1)) return 1;
                lua_pop(L,2);
            }
            lua_pop(L,1);
            const auto& value=read_property(L,1,key);
            if(value.index()>=4) push_field(L,1,{key}); else push_attribute(L,value);
            return 1;
        } catch (const std::exception& e) { return luaL_error(L,"%s",e.what()); }
    }
    using FieldPart=std::variant<std::string,std::size_t>;
    struct FieldProxy { Impl* engine; std::vector<FieldPart> path; std::size_t charge=4096; };
    static const AttributeValue& read_property(lua_State* L,int index,const std::string& key) {
        object_check(L,index); auto& object=attribute_object(L,index);
        if(object.mode==ObjectMode::replica || object.mode==ObjectMode::prediction) {
            if(!object.present.contains(key)) throw std::logic_error("property is absent from this projection");
            auto access=object.model.schema().attributes.at(key).client;
            if(access!=Access::read && access!=Access::read_write) throw std::logic_error("property read denied");
            return object.model.all_values().at(key);
        }
        if(object.mode==ObjectMode::local) return object.model.all_values().at(key);
        return object.model.read(key,{Caller::server,{}});
    }
    static const AttributeValue& field_child(const AttributeValue& value,const FieldPart& part) {
        if(auto name=std::get_if<std::string>(&part)) return value_members(value)->at(*name);
        return std::get<AttributeArray>(value).at(std::get<std::size_t>(part));
    }
    static AttributeValue& field_child(AttributeValue& value,const FieldPart& part) {
        if(auto name=std::get_if<std::string>(&part)) return value_members(value)->at(*name);
        return std::get<AttributeArray>(value).at(std::get<std::size_t>(part));
    }
    static const AttributeDefinition& field_definition(const AttributeDefinition& d,const FieldPart& part) {
        if(auto name=std::get_if<std::string>(&part)) return d.type==AttributeType::map?*d.element:d.fields.at(*name);
        auto index=std::get<std::size_t>(part);
        if(!d.element || index>=(d.type==AttributeType::array?d.length:d.max_items)) throw std::out_of_range("array index out of range");
        return *d.element;
    }
    static const AttributeValue& resolve_field(lua_State* L,int root,const FieldProxy& proxy) {
        auto* value=&read_property(L,root,std::get<std::string>(proxy.path.at(0)));
        for(std::size_t i=1;i<proxy.path.size();++i) value=&field_child(*value,proxy.path[i]);
        return *value;
    }
    static AttributeValue proxy_value(lua_State* L,int index) {
        auto& proxy=*static_cast<FieldProxy*>(luaL_checkudata(L,index,"fsnet.field"));
        lua_getiuservalue(L,index,1);
        auto value=resolve_field(L,lua_gettop(L),proxy); lua_pop(L,1); return value;
    }
    static void push_field(lua_State* L,int root,std::vector<FieldPart> path) {
        root=lua_absindex(L,root); auto& engine=self(L);
        if(path.size()>9) throw std::length_error("field view depth budget");
        // Native path allocations are charged more than Lua's userdata header.
        // Collect dead views before rejecting a budget Lua GC cannot otherwise see.
        if(engine.limits.memory_bytes-engine.used<4096) lua_gc(L,LUA_GCCOLLECT);
        if(engine.limits.memory_bytes-engine.used<4096) throw std::length_error("field view memory budget");
        void* memory=lua_newuserdatauv(L,sizeof(FieldProxy),1);
        if(engine.limits.memory_bytes-engine.used<4096) lua_gc(L,LUA_GCCOLLECT);
        if(engine.limits.memory_bytes-engine.used<4096) throw std::length_error("field view memory budget");
        new(memory) FieldProxy{&engine,std::move(path),4096}; engine.used+=4096;
        luaL_setmetatable(L,"fsnet.field"); lua_pushvalue(L,root); lua_setiuservalue(L,-2,1);
    }
    static FieldPart field_key(lua_State* L,int index,const AttributeValue& parent) {
        if(value_members(parent)) {
            if(lua_type(L,index)!=LUA_TSTRING) throw std::invalid_argument("struct field must be a name");
            std::size_t n; const char* key=lua_tolstring(L,index,&n); return std::string(key,n);
        }
        if(!lua_isinteger(L,index) || lua_tointeger(L,index)<1) throw std::invalid_argument("array index must be positive integer");
        return static_cast<std::size_t>(lua_tointeger(L,index)-1);
    }
    static int field_gc(lua_State* L) {
        auto& proxy=*static_cast<FieldProxy*>(luaL_checkudata(L,1,"fsnet.field"));
        proxy.engine->used-=proxy.charge; proxy.~FieldProxy(); return 0;
    }
    static const AttributeDefinition& proxy_definition(const AttributeObject& object,const FieldProxy& proxy) {
        auto* definition=&object.model.schema().attributes.at(std::get<std::string>(proxy.path.at(0)));
        for(std::size_t i=1;i<proxy.path.size();++i) definition=&field_definition(*definition,proxy.path[i]);
        return *definition;
    }
    static int field_index(lua_State* L) {
        try {
            auto& proxy=*static_cast<FieldProxy*>(luaL_checkudata(L,1,"fsnet.field"));
            lua_getiuservalue(L,1,1); int root=lua_gettop(L);
            const auto& parent=resolve_field(L,root,proxy);
            const auto& definition=proxy_definition(attribute_object(L,root),proxy);
            if((definition.type==AttributeType::list || definition.type==AttributeType::set) && lua_type(L,2)==LUA_TSTRING) {
                std::string method=lua_tostring(L,2);
                if(method=="add" || method=="remove" || method=="clear" || method=="contains" || (method=="insert" && definition.type==AttributeType::list)) {
                    lua_pushvalue(L,2); lua_pushcclosure(L,container_method,1); return 1;
                }
            }
            auto part=field_key(L,2,parent);
            if(auto entries=std::get_if<AttributeMap>(&parent); entries && !entries->contains(std::get<std::string>(part))) { lua_pushnil(L); return 1; }
            if(auto items=std::get_if<AttributeArray>(&parent); items && std::get<std::size_t>(part)>=items->size()) { lua_pushnil(L); return 1; }
            const auto& child=field_child(parent,part);
            if(child.index()<4) push_attribute(L,child);
            else { auto path=proxy.path; path.push_back(std::move(part)); push_field(L,root,std::move(path)); }
            return 1;
        } catch(const std::exception& e) { return luaL_error(L,"%s",e.what()); }
    }
    static int field_length(lua_State* L) {
        try {
            auto& proxy=*static_cast<FieldProxy*>(luaL_checkudata(L,1,"fsnet.field"));
            lua_getiuservalue(L,1,1); const auto& value=resolve_field(L,lua_gettop(L),proxy);
            auto size=std::holds_alternative<AttributeArray>(value)?std::get<AttributeArray>(value).size():value_members(value)->size();
            lua_pushinteger(L,static_cast<lua_Integer>(size)); return 1;
        } catch(const std::exception& e) { return luaL_error(L,"%s",e.what()); }
    }
    static int field_next(lua_State* L) {
        try {
            auto& proxy=*static_cast<FieldProxy*>(luaL_checkudata(L,1,"fsnet.field"));
            lua_getiuservalue(L,1,1); int root=lua_gettop(L);
            const auto& parent=resolve_field(L,root,proxy);
            if(auto fields=value_members(parent)) {
                auto it=fields->begin();
                if(!lua_isnil(L,2)) { std::size_t n; const char* key=luaL_checklstring(L,2,&n); it=fields->upper_bound(std::string(key,n)); }
                if(it==fields->end()) return 0;
                lua_pushlstring(L,it->first.data(),it->first.size());
            } else {
                auto index=lua_isnil(L,2)?1:luaL_checkinteger(L,2)+1;
                if(index<1 || static_cast<std::size_t>(index)>std::get<AttributeArray>(parent).size()) return 0;
                lua_pushinteger(L,index);
            }
            lua_replace(L,2); lua_settop(L,2); // field_index performs a fresh checked read
            field_index(L); lua_pushvalue(L,2); lua_insert(L,-2); return 2;
        } catch(const std::exception& e) { return luaL_error(L,"%s",e.what()); }
    }
    static int field_pairs(lua_State* L) { lua_pushcfunction(L,field_next); lua_pushvalue(L,1); lua_pushnil(L); return 3; }
    static bool set_property(lua_State* L,int root,const std::string& key,AttributeValue value) {
        object_check(L,root); auto& object=attribute_object(L,root); auto& engine=*object.engine;
        if(engine.predicting && engine.predicting!=&object) { engine.prediction_violation=true; throw std::logic_error("prediction cannot mutate other objects"); }
        if(object.mode==ObjectMode::prediction && engine.predicting!=&object) throw std::logic_error("prediction state is writable only during input execution");
        if(object.mode==ObjectMode::prediction && !object.present.contains(key)) throw std::logic_error("property is absent from this projection");
        const auto& d=object.model.schema().attributes.at(key);
        if(object.mode==ObjectMode::replica) {
            prediction_external(L);
            if(!engine.threads.contains(L) || !object.subscription) throw std::logic_error("client assignment requires an async task and live subscription");
            if(d.client!=Access::write && d.client!=Access::read_write) throw std::logic_error("confirmed property write denied");
            // Validate a complete candidate without changing confirmed state.
            auto candidate=object.model;
            candidate.publish({candidate.revision(),{{key,value}}});
            Await request{}; request.kind=Await::Kind::write; request.timeout=std::chrono::milliseconds(3000);
            request.subscription=object.subscription; request.values={{key,std::move(value)}};
            request.options.expected_revision=object.model.revision();
            engine.tasks.at(engine.threads.at(L)).await=std::move(request); return true;
        }
        if((object.mode==ObjectMode::local || object.mode==ObjectMode::prediction) && !d.driver_authority) throw std::logic_error("local write requires driver authority");
        auto mutation=object.mode==ObjectMode::request?object.model.stage({{key,std::move(value)}},{Caller::server,{}})
            :EntityAttributes::Mutation{object.model.revision(),{{key,std::move(value)}}};
        auto projected=object.model.all_values(); projected.at(key)=mutation.values.at(key);
        (void)attribute_wire_size(projected);
        auto charge=object.fixed_charge+4*attribute_image_bytes(projected);
        if(charge>object.charge && charge-object.charge>engine.limits.memory_bytes-engine.used) throw std::length_error("attribute object memory limit");
        if(!object.model.publish(mutation)) throw std::logic_error("stale attribute object");
        engine.used=engine.used-object.charge+charge; object.charge=charge; return false;
    }
    static int assignment_complete(lua_State* L,int,lua_KContext) {
        auto status=lua_tointeger(L,-1);
        if(status!=static_cast<int>(Status::ok)) return luaL_error(L,"property assignment failed: %d",static_cast<int>(status));
        return 0;
    }
    static int field_newindex(lua_State* L) {
        bool waiting=false;
        try {
            auto& proxy=*static_cast<FieldProxy*>(luaL_checkudata(L,1,"fsnet.field"));
            lua_getiuservalue(L,1,1); int root=lua_gettop(L);
            const auto& top=std::get<std::string>(proxy.path.at(0));
            auto value=read_property(L,root,top);
            auto* parent=&value;
            auto* definition=&attribute_object(L,root).model.schema().attributes.at(top);
            for(std::size_t i=1;i<proxy.path.size();++i) {
                parent=&field_child(*parent,proxy.path[i]); definition=&field_definition(*definition,proxy.path[i]);
            }
            auto part=field_key(L,2,*parent);
            if(definition->type==AttributeType::map) {
                auto& entries=std::get<AttributeMap>(*parent); const auto& key=std::get<std::string>(part);
                if(lua_isnil(L,3)) entries.erase(key);
                else entries.insert_or_assign(key,attribute_value(L,3,*definition->element));
            } else {
                if(definition->type==AttributeType::set) throw std::logic_error("use set add/remove to change membership");
                auto replacement=attribute_value(L,3,field_definition(*definition,part));
                if(definition->type==AttributeType::list && std::get<std::size_t>(part)==std::get<AttributeArray>(*parent).size())
                    std::get<AttributeArray>(*parent).push_back(std::move(replacement));
                else field_child(*parent,part)=std::move(replacement);
            }
            waiting=set_property(L,root,top,std::move(value));
        } catch(const std::exception& e) { return luaL_error(L,"%s",e.what()); }
        return waiting?lua_yieldk(L,0,0,assignment_complete):0;
    }
    static int container_method(lua_State* L) {
        bool waiting=false;
        try {
            auto& proxy=*static_cast<FieldProxy*>(luaL_checkudata(L,1,"fsnet.field"));
            lua_getiuservalue(L,1,1); int root=lua_gettop(L);
            const auto& top=std::get<std::string>(proxy.path.at(0));
            auto value=read_property(L,root,top); auto* container=&value;
            for(std::size_t i=1;i<proxy.path.size();++i) container=&field_child(*container,proxy.path[i]);
            const auto& definition=proxy_definition(attribute_object(L,root),proxy);
            if(definition.type!=AttributeType::list && definition.type!=AttributeType::set) throw std::logic_error("expected list or set");
            auto& items=std::get<AttributeArray>(*container);
            std::string method=lua_tostring(L,lua_upvalueindex(1));
            if(method=="clear") items.clear();
            else if(method=="remove" && definition.type==AttributeType::list) {
                if(!lua_isinteger(L,2) || lua_tointeger(L,2)<1 || static_cast<std::size_t>(lua_tointeger(L,2))>items.size()) throw std::out_of_range("list removal index");
                items.erase(items.begin()+lua_tointeger(L,2)-1);
            } else {
                auto child=attribute_value(L,method=="insert"?3:2,*definition.element);
                auto found=std::find(items.begin(),items.end(),child);
                if(method=="contains") { lua_pushboolean(L,found!=items.end()); return 1; }
                if(method=="remove") { if(found!=items.end()) items.erase(found); }
                else if(method=="insert") {
                    if(!lua_isinteger(L,2) || lua_tointeger(L,2)<1 || static_cast<std::size_t>(lua_tointeger(L,2))>items.size()+1) throw std::out_of_range("list insertion index");
                    items.insert(items.begin()+lua_tointeger(L,2)-1,std::move(child));
                } else if(definition.type!=AttributeType::set || found==items.end()) items.push_back(std::move(child));
            }
            waiting=set_property(L,root,top,std::move(value));
        } catch(const std::exception& e) { return luaL_error(L,"%s",e.what()); }
        return waiting?lua_yieldk(L,0,0,assignment_complete):0;
    }
    static int attributes_newindex(lua_State* L) {
        bool waiting=false;
        try {
            std::size_t n; const char* text=luaL_checklstring(L,2,&n); std::string key(text,n);
            object_check(L,1); auto& object=attribute_object(L,1);
            auto value=attribute_value(L,3,object.model.schema().attributes.at(key));
            waiting=set_property(L,1,key,std::move(value));
        } catch(const std::exception& e) { return luaL_error(L,"%s",e.what()); }
        return waiting?lua_yieldk(L,0,0,assignment_complete):0;
    }
    static int reject_request(lua_State* L) {
        prediction_external(L);
        auto& engine = self(L);
        if (!engine.threads.contains(L)) return luaL_error(L, "reject requires an RPC coroutine");
        engine.tasks.at(engine.threads.at(L)).rejected = true;
        return luaL_error(L, "request rejected by script");
    }
    static int attributes_proposal(lua_State* L) {
        prediction_external(L);
        std::size_t size; const char* input = luaL_checklstring(L, 2, &size);
        try {
            auto& engine = self(L); auto& object = attribute_object(L, 1);
            auto proposal = decode_attributes(std::string_view(input,size));
            if (!engine.threads.contains(L) || proposal.snapshot || proposal.base != object.model.revision() || proposal.revision != proposal.base)
                throw std::invalid_argument("invalid or stale client proposal");
            auto mutation = object.model.stage(proposal.values, engine.tasks.at(engine.threads.at(L)).identity);
            lua_createtable(L, 0, static_cast<int>(mutation.values.size()));
            for (const auto& [name,value] : mutation.values) {
                lua_pushlstring(L,name.data(),name.size()); push_attribute(L,value); lua_settable(L,-3);
            }
            return 1;
        } catch (const std::exception&) { return reject_request(L); }
    }
    static int attributes_apply(lua_State* L) {
        prediction_external(L);
        std::size_t size; const char* input = luaL_checklstring(L, 2, &size);
        try {
            auto& engine = self(L); auto& object = attribute_object(L, 1);
            auto proposal = decode_attributes(std::string_view(input,size));
            if (!engine.threads.contains(L) || proposal.snapshot || proposal.base != object.model.revision() || proposal.revision != proposal.base)
                throw std::invalid_argument("invalid or stale client proposal");
            auto mutation = object.model.stage(proposal.values, engine.tasks.at(engine.threads.at(L)).identity);
            auto projected = object.model.all_values();
            for (const auto& [name,value] : mutation.values) projected.at(name) = value;
            (void)attribute_wire_size(projected);
            auto charge = object.fixed_charge+4*attribute_image_bytes(projected);
            if (charge > object.charge && charge - object.charge > engine.limits.memory_bytes - engine.used)
                throw std::length_error("attribute object memory limit");
            if (!object.model.publish(mutation)) throw std::runtime_error("stale client mutation");
            engine.used = engine.used - object.charge + charge; object.charge = charge; return 0;
        } catch (const std::exception&) { return reject_request(L); }
    }
    static int attributes_close(lua_State* L) {
        try {
            auto& model = attribute_object(L, 1).model;
            auto state = encode_attributes({0, model.revision(), true, model.all_values()});
            lua_pushlstring(L, state.data(), state.size()); return 1;
        } catch (const std::exception& e) { return luaL_error(L, "%s", e.what()); }
    }
    static EntityAttributes::Values values(lua_State* L,int index,const EntitySchema& schema,bool driver_only=false) {
        luaL_checktype(L,index,LUA_TTABLE); index=lua_absindex(L,index);
        EntityAttributes::Values result;
        lua_pushnil(L);
        while(lua_next(L,index)) {
            if(lua_type(L,-2)!=LUA_TSTRING || result.size()>=256) throw std::invalid_argument("named property values required");
            std::size_t size; const char* key=lua_tolstring(L,-2,&size); std::string name(key,size);
            const auto& d=schema.attributes.at(name);
            if(driver_only && !d.driver_authority) throw std::logic_error("local write requires driver authority");
            result.emplace(std::move(name),attribute_value(L,-1,d)); lua_pop(L,1);
        }
        return result;
    }
    static int model_publish(lua_State* L) {
        auto& engine=self(L); std::size_t n; const char* kind=luaL_checklstring(L,1,&n);
        if(!engine.loading) return luaL_error(L,"publish model only during load");
        try {
            auto name=std::string(kind,n); const auto& schema=engine.schemas.at(name);
            auto initial=encode_attributes({0,0,true,EntityAttributes(schema,"").persistent()});
            engine.server_schemas.insert(name); lua_pushlstring(L,initial.data(),initial.size()); return 1;
        } catch(const std::exception& e) { return luaL_error(L,"%s",e.what()); }
    }
    static int object_create(lua_State* L) {
        prediction_external(L);
        auto& engine=self(L); std::size_t n; const char* kind=luaL_checklstring(L,1,&n);
        bool replica=lua_toboolean(L,2); auto subscription=luaL_optinteger(L,3,0);
        try {
            if(subscription<0) throw std::invalid_argument("invalid subscription");
            std::string type(kind,n); const auto& schema=engine.schemas.at(type);
            EntityAttributes model(schema,""); std::unordered_set<std::string> present;
            std::shared_ptr<const AttributeUpdate> projection;
            if(replica) {
                auto image=engine.host->attribute_snapshot(static_cast<std::uint64_t>(subscription));
                if(!image) throw std::logic_error("attribute subscription was revoked");
                model.restore_full(image->values,image->revision);
                for(const auto& [key,value]:image->values) { (void)value; present.insert(key); }
                projection=std::move(image);
            } else if(!lua_isnoneornil(L,4)) {
                auto mutation=EntityAttributes::Mutation{0,values(L,4,schema,true)};
                if(!model.publish(mutation)) throw std::logic_error("stale local initialization");
            }
            auto encoded=encode_attributes({0,model.revision(),true,model.all_values()});
            auto charge=carrier_charge(model);
            if(charge>engine.limits.memory_bytes-engine.used) throw std::length_error("object memory budget");
            void* memory=lua_newuserdatauv(L,sizeof(AttributeObject),1);
            if(charge>engine.limits.memory_bytes-engine.used) throw std::length_error("object memory budget");
            auto* object=new(memory) AttributeObject{&engine,charge,std::move(model),0,replica?ObjectMode::replica:ObjectMode::local,static_cast<std::uint64_t>(subscription),std::move(present),std::move(type),std::move(projection)};
            object->fixed_charge=charge-4*attribute_image_bytes(object->model.all_values());
            engine.used+=charge; luaL_setmetatable(L,"fsnet.attributes"); return 1;
        } catch(const std::exception& e) { return luaL_error(L,"%s",e.what()); }
    }
    static int prediction_guard(lua_State* L) { prediction_external(L); return 0; }
    static int prediction_clone(lua_State* L) {
        prediction_external(L);
        try {
            object_check(L,1,true);
            auto& source=attribute_object(L,1);
            if(source.mode!=ObjectMode::replica && source.mode!=ObjectMode::prediction)
                throw std::logic_error("prediction requires a confirmed replica");
            EntityAttributes model(source.model.schema(),"");
            model.restore_full(source.model.all_values(),source.model.revision());
            auto charge=carrier_charge(model);
            auto& engine=self(L);
            if(charge>engine.limits.memory_bytes-engine.used) throw std::length_error("prediction memory budget");
            void* memory=lua_newuserdatauv(L,sizeof(AttributeObject),1);
            if(charge>engine.limits.memory_bytes-engine.used) throw std::length_error("prediction memory budget");
            auto* object=new(memory) AttributeObject{&engine,charge,std::move(model),0,ObjectMode::prediction,0,source.present,source.kind,{}};
            object->fixed_charge=charge-4*attribute_image_bytes(object->model.all_values());
            engine.used+=charge; luaL_setmetatable(L,"fsnet.attributes");
            lua_getiuservalue(L,1,1);
            lua_newtable(L);
            for(const char* field : {"members","id","type","context"}) {
                lua_getfield(L,-2,field); lua_setfield(L,-2,field);
            }
            if(source.mode==ObjectMode::replica) lua_pushvalue(L,1);
            else lua_getfield(L,-2,"confirmed");
            lua_setfield(L,-2,"confirmed");
            lua_pushboolean(L,true); lua_setfield(L,-2,"active");
            lua_setiuservalue(L,-3,1); lua_pop(L,1);
            return 1;
        } catch(const std::exception& e) { return luaL_error(L,"%s",e.what()); }
    }
    static int prediction_run(lua_State* L) {
        prediction_external(L);
        luaL_checktype(L,2,LUA_TFUNCTION);
        try {
            object_check(L,1,true);
            auto& object=attribute_object(L,1);
            if(object.mode!=ObjectMode::prediction) throw std::logic_error("expected prediction state");
            auto& engine=self(L);
            struct Scope {
                Impl& engine;
                ~Scope() { engine.predicting=nullptr; }
            } scope{engine};
            // A child coroutine keeps the instruction hook yieldable, including
            // inside script pcall loops. A non-yieldable lua_pcall C boundary
            // would turn budget yields into errors that gameplay could swallow.
            auto* thread=lua_newthread(L); // rooted on the parent's stack
            lua_sethook(thread,hook,LUA_MASKCOUNT,1000);
            lua_pushvalue(L,2); lua_pushvalue(L,1); lua_pushvalue(L,3);
            lua_xmove(L,thread,3);
            engine.predicting=&object; engine.prediction_violation=false;
            int results=0;
            auto status=lua_resume(thread,L,2,&results);
            engine.predicting=nullptr;
            if(engine.prediction_violation) return luaL_error(L,"prediction attempted a forbidden effect");
            if(engine.exhausted || status==LUA_YIELD) return luaL_error(L,"prediction instruction budget exceeded");
            if(status!=LUA_OK) { lua_xmove(thread,L,1); return lua_error(L); }
            if(results>1) return luaL_error(L,"prediction method must return bytes or nil");
            if(results==0) lua_pushnil(L); else lua_xmove(thread,L,1);
            if(!lua_isnil(L,-1) && lua_type(L,-1)!=LUA_TSTRING) return luaL_error(L,"prediction method must return bytes or nil");
            return 1;
        } catch(const std::exception& e) { return luaL_error(L,"%s",e.what()); }
    }
    static int object_dispose(lua_State* L) {
        prediction_external(L);
        auto& object=attribute_object(L,1);
        if(object.mode==ObjectMode::request) return luaL_error(L,"server objects are scoped by their request");
        if(object.subscription) {
            try { object.engine->host->unwatch_attributes(object.subscription); }
            catch(const std::exception& e) { return luaL_error(L,"%s",e.what()); }
            object.subscription=0;
        }
        lua_getiuservalue(L,1,1);
        if(lua_istable(L,-1)) { lua_pushboolean(L,false); lua_setfield(L,-2,"active"); }
        lua_pop(L,1); return 0;
    }
    static int unwatch_attributes(lua_State* L) {
        prediction_external(L);
        auto subscription=luaL_checkinteger(L,1);
        try { if(subscription>0) self(L).host->unwatch_attributes(static_cast<std::uint64_t>(subscription)); return 0; }
        catch(const std::exception& e) { return luaL_error(L,"%s",e.what()); }
    }
    static int start_task(lua_State* L) {
        prediction_external(L);
        auto& engine=self(L); luaL_checktype(L,1,LUA_TFUNCTION);
        if(!engine.loading || engine.starts.size()>=engine.limits.queued_requests) return luaL_error(L,"start tasks only during load, within queue limit");
        lua_pushvalue(L,1); engine.starts.push_back(luaL_ref(L,LUA_REGISTRYINDEX)); return 0;
    }
    static int sleep_task(lua_State* L) {
        prediction_external(L);
        auto& engine=self(L); auto duration=luaL_checkinteger(L,1);
        if(duration<1 || duration>30000 || !engine.threads.contains(L)) return luaL_error(L,"sleep requires an async task and 1..30000 ms");
        Await request{}; request.kind=Await::Kind::sleep; request.timeout=std::chrono::milliseconds(duration);
        engine.tasks.at(engine.threads.at(L)).await=std::move(request); return lua_yield(L,0);
    }
    static int watch_attributes(lua_State* L) {
        prediction_external(L);
        auto& engine=self(L); std::size_t kind_size,id_size;
        const char* kind=luaL_checklstring(L,1,&kind_size); const char* id=luaL_checklstring(L,2,&id_size);
        auto timeout=luaL_optinteger(L,3,3000);
        if(!engine.threads.contains(L) || timeout<1 || timeout>30000) return luaL_error(L,"watch requires an async task and a bounded deadline");
        try {
            Await request{}; request.kind=Await::Kind::watch; request.timeout=std::chrono::milliseconds(timeout);
            request.route=std::string(kind,kind_size); request.data=std::string(id,id_size);
            engine.tasks.at(engine.threads.at(L)).await=std::move(request);
        } catch(const std::exception& e) { return luaL_error(L,"%s",e.what()); }
        return lua_yield(L,0);
    }
    static int write_attributes(lua_State* L) {
        prediction_external(L);
        auto& engine=self(L); auto& object=attribute_object(L,1);
        auto timeout=luaL_optinteger(L,5,3000);
        std::size_t n; const char* token=luaL_optlstring(L,4,"",&n);
        if(!engine.threads.contains(L) || timeout<1 || timeout>30000) return luaL_error(L,"write requires an async task and a bounded deadline");
        try {
            object_check(L,1,true);
            if(object.mode!=ObjectMode::replica || !object.subscription) throw std::logic_error("write requires a live confirmed replica");
            auto revision=luaL_optinteger(L,3,static_cast<lua_Integer>(object.model.revision()));
            if(revision<0 || n>128) throw std::invalid_argument("invalid write revision/request ID");
            Await request{}; request.kind=Await::Kind::write; request.timeout=std::chrono::milliseconds(timeout);
            request.subscription=object.subscription; request.values=values(L,2,object.model.schema());
            request.options={static_cast<std::uint64_t>(revision),std::string(token,n)};
            engine.tasks.at(engine.threads.at(L)).await=std::move(request);
        } catch(const std::exception& e) { return luaL_error(L,"%s",e.what()); }
        return lua_yield(L,0);
    }

    static int spawn_entity(lua_State* L) {
        prediction_external(L);
        auto& engine=self(L);
        std::size_t n,m; const char* kind=luaL_checklstring(L,1,&n); const char* id=luaL_checklstring(L,2,&m);
        if(!engine.threads.contains(L)) return luaL_error(L,"spawn requires an async server task");
        try {
            std::string type(kind,n);
            if(!engine.server_schemas.contains(type) || m==0 || m>256) throw std::invalid_argument("spawn requires a registered server class and valid ID");
            const auto& task=engine.tasks.at(engine.threads.at(L));
            if(task.entity_type==type && task.entity_id==std::string_view(id,m)) throw std::logic_error("cannot spawn the currently executing entity");
            Await request{}; request.kind=Await::Kind::spawn; request.timeout=engine.limits.task_timeout;
            request.route=std::move(type); request.data=std::string(id,m);
            request.values=values(L,3,engine.schemas.at(request.route));
            engine.tasks.at(engine.threads.at(L)).await=std::move(request);
        } catch(const std::exception& e) { return luaL_error(L,"%s",e.what()); }
        return lua_yield(L,0);
    }
    static int entity_visibility(lua_State* L) {
        prediction_external(L);
        auto& engine=self(L);
        auto peer=luaL_checkinteger(L,1),action=luaL_checkinteger(L,4);
        std::size_t n,m; const char* kind=luaL_checklstring(L,2,&n); const char* id=luaL_checklstring(L,3,&m);
        if(!engine.threads.contains(L) || peer<=0 || action<1 || action>4) return luaL_error(L,"visibility requires an async server task and valid peer/action");
        try {
            std::string type(kind,n);
            if(!engine.server_schemas.contains(type)) throw std::invalid_argument("visibility requires a registered server class");
            const auto& task=engine.tasks.at(engine.threads.at(L));
            if(action==1 && task.entity_type==type && task.entity_id==std::string_view(id,m)) throw std::logic_error("show must run outside the target entity mailbox");
            Await request{}; request.kind=Await::Kind::visibility; request.timeout=engine.limits.task_timeout;
            request.peer=static_cast<std::uint64_t>(peer); request.route=std::move(type); request.data=std::string(id,m); request.action=static_cast<unsigned>(action);
            engine.tasks.at(engine.threads.at(L)).await=std::move(request);
        } catch(const std::exception& e) { return luaL_error(L,"%s",e.what()); }
        return lua_yield(L,0);
    }
    static int stop_entity_events(lua_State* L) {
        prediction_external(L);
        try { self(L).host->stop_entity_events(); return 0; }
        catch(const std::exception& e) { return luaL_error(L,"%s",e.what()); }
    }
    static int subscription_alive(lua_State* L) {
        auto id=luaL_checkinteger(L,1);
        try { lua_pushboolean(L,id>0 && bool(self(L).host->attribute_snapshot(static_cast<std::uint64_t>(id)))); return 1; }
        catch(const std::exception& e) { return luaL_error(L,"%s",e.what()); }
    }
    static int entity_event(lua_State* L) {
        prediction_external(L);
        try {
            auto event=self(L).host->pop_entity_event();
            if(!event) { lua_pushnil(L); return 1; }
            lua_createtable(L,0,6);
            lua_pushinteger(L,static_cast<unsigned>(event->kind)); lua_setfield(L,-2,"action");
            lua_pushlstring(L,event->type.data(),event->type.size()); lua_setfield(L,-2,"type");
            lua_pushlstring(L,event->id.data(),event->id.size()); lua_setfield(L,-2,"id");
            lua_pushinteger(L,static_cast<lua_Integer>(event->subscription)); lua_setfield(L,-2,"subscription");
            lua_pushboolean(L,event->owned); lua_setfield(L,-2,"owned");
            lua_pushboolean(L,event->controlled); lua_setfield(L,-2,"controlled");
            return 1;
        } catch(const std::exception& e) { return luaL_error(L,"%s",e.what()); }
    }
    static int rpc(lua_State* L) {
        luaL_checktype(L, 1, LUA_TSTRING); luaL_checktype(L, 2, LUA_TFUNCTION);
        std::size_t size = 0; const char* name = lua_tolstring(L, 1, &size);
        auto& owner = self(L);
        if (!owner.loading) return luaL_error(L, "register routes only during load");
        if (owner.routes.size() >= owner.limits.registered_routes) return luaL_error(L, "script route limit exceeded");
        if (!size || size > 256 || std::string_view(name, size).find('\0') != std::string_view::npos)
            return luaL_error(L, "invalid RPC route");
        try {
            std::string route(name, size);
            if (owner.routes.contains(route)) return luaL_error(L, "duplicate RPC route");
            std::string entity_type;
            if (!lua_isnoneornil(L, 3)) {
                std::size_t length; const char* type = luaL_checklstring(L, 3, &length);
                if (!length || length > 127 || std::string_view(type, length).find_first_of(".\0", 0, 2) != std::string_view::npos)
                    return luaL_error(L, "invalid entity type");
                entity_type.assign(type, length);
            }
            std::optional<std::string> initial_state;
            if (!lua_isnoneornil(L, 4)) {
                if (entity_type.empty()) return luaL_error(L, "entity type required");
                std::size_t length; const char* initial = luaL_checklstring(L, 4, &length);
                if (length > owner.limits.queued_bytes) return luaL_error(L, "initial state too large");
                initial_state = std::string(initial, length);
            }
            std::optional<RpcPolicy> policy;
            if (!lua_isnoneornil(L, 5)) {
                luaL_checktype(L, 5, LUA_TTABLE);
                RpcPolicy parsed;
                lua_getfield(L, 5, "client"); parsed.client = lua_toboolean(L, -1); lua_pop(L, 1);
                lua_getfield(L, 5, "server"); parsed.server = lua_toboolean(L, -1); lua_pop(L, 1);
                lua_getfield(L, 5, "owner_only"); parsed.owner_only = lua_toboolean(L, -1); lua_pop(L, 1);
                if (!parsed.client && !parsed.server) return luaL_error(L, "explicit RPC callers required");
                policy = parsed;
            }
            lua_pushvalue(L, 2);
            int reference = luaL_ref(L, LUA_REGISTRYINDEX);
            try {
                if (!entity_type.empty()) owner.entity_routes.emplace(route, std::move(entity_type));
                if (initial_state) owner.stateful_routes.emplace(route, std::move(*initial_state));
                owner.routes.emplace(route, reference);
                if (policy) owner.policies.emplace(route, *policy);
            } catch (const std::exception&) {
                owner.entity_routes.erase(route); owner.stateful_routes.erase(route); owner.policies.erase(route); owner.routes.erase(route);
                luaL_unref(L, LUA_REGISTRYINDEX, reference); throw;
            }
            return 0;
        } catch (const std::exception& error) { return luaL_error(L, "%s", error.what()); }
    }
    static int connect(lua_State* L) {
        prediction_external(L);
        luaL_checktype(L, 1, LUA_TSTRING);
        auto port = luaL_checkinteger(L, 2);
        if (port <= 0 || port > 65535) return luaL_error(L, "invalid port");
        auto& owner = self(L);
        if (owner.loading || !owner.threads.contains(L)) return luaL_error(L, "connect requires an RPC coroutine");
        std::size_t size; const char* address = lua_tolstring(L, 1, &size);
        try {
            auto peer = owner.host->connect(std::string(address, size), static_cast<std::uint16_t>(port));
            if (peer > static_cast<std::uint64_t>(std::numeric_limits<lua_Integer>::max()))
                return luaL_error(L, "peer handle exhausted");
            lua_pushinteger(L, static_cast<lua_Integer>(peer)); return 1;
        } catch (const std::exception& error) { return luaL_error(L, "%s", error.what()); }
    }
    static int request(lua_State* L) {
        prediction_external(L);
        auto peer = luaL_checkinteger(L, 1);
        luaL_checktype(L, 2, LUA_TSTRING); luaL_checktype(L, 3, LUA_TSTRING);
        auto timeout = luaL_optinteger(L, 4, 1000);
        if (peer < 0 || timeout <= 0 || timeout > 30000) return luaL_error(L, "invalid peer or deadline");
        auto& owner = self(L);
        auto it = owner.threads.find(L);
        if (owner.loading || it == owner.threads.end()) return luaL_error(L, "request requires an RPC coroutine");
        std::size_t route_size, size;
        const char* route = lua_tolstring(L, 2, &route_size);
        const char* data = lua_tolstring(L, 3, &size);
        if (size > owner.limits.queued_bytes) return luaL_error(L, "RPC payload exceeds host byte budget");
        if (!route_size || route_size > 256 || std::string_view(route, route_size).find('\0') != std::string_view::npos)
            return luaL_error(L, "invalid RPC route");
        try {
            Await request{}; request.peer=static_cast<std::uint64_t>(peer);
            request.route=std::string(route,route_size); request.data=std::string(data,size); request.timeout=std::chrono::milliseconds(timeout);
            owner.tasks.at(it->second).await=std::move(request);
        } catch (const std::exception& error) { return luaL_error(L, "%s", error.what()); }
        return lua_yield(L, 0);
    }
    static void hook(lua_State* L, lua_Debug*) {
        auto& owner = self(L);
        if (owner.budget <= 1000) {
            owner.exhausted = true;
            // A yield cannot be swallowed by a script pcall error handler.
            lua_yield(L, 0);
        } else owner.budget -= 1000;
    }
    static int initialize(lua_State* L) {
        luaL_requiref(L, LUA_GNAME, luaopen_base, 1); lua_pop(L, 1);
        luaL_requiref(L, LUA_TABLIBNAME, luaopen_table, 1); lua_pop(L, 1);
        luaL_requiref(L, LUA_STRLIBNAME, luaopen_string, 1); lua_pop(L, 1);
        luaL_requiref(L, LUA_MATHLIBNAME, luaopen_math, 1); lua_pop(L, 1);
        luaL_requiref(L, LUA_UTF8LIBNAME, luaopen_utf8, 1); lua_pop(L, 1);
        for (const char* name : {"dofile", "loadfile", "load", "collectgarbage"}) { lua_pushnil(L); lua_setglobal(L, name); }
        lua_newtable(L);
        lua_pushcfunction(L,start_task); lua_setfield(L,-2,"_start");
        lua_pushcfunction(L,sleep_task); lua_setfield(L,-2,"_sleep");
        lua_pushcfunction(L,watch_attributes); lua_setfield(L,-2,"_watch");
        lua_pushcfunction(L,unwatch_attributes); lua_setfield(L,-2,"_unwatch");
        lua_pushcfunction(L,write_attributes); lua_setfield(L,-2,"_write");
        lua_pushcfunction(L,spawn_entity); lua_setfield(L,-2,"_spawn_entity");
        lua_pushcfunction(L,entity_visibility); lua_setfield(L,-2,"_entity_visibility");
        lua_pushcfunction(L,stop_entity_events); lua_setfield(L,-2,"_stop_entity_events");
        lua_pushcfunction(L,subscription_alive); lua_setfield(L,-2,"_subscription_alive");
        lua_pushcfunction(L,entity_event); lua_setfield(L,-2,"_entity_event");
        lua_pushcfunction(L,model_publish); lua_setfield(L,-2,"_model_publish");
        lua_pushcfunction(L,prediction_guard); lua_setfield(L,-2,"_prediction_guard");
        lua_pushcfunction(L,prediction_clone); lua_setfield(L,-2,"_prediction_clone");
        lua_pushcfunction(L,prediction_run); lua_setfield(L,-2,"_prediction_run");
        lua_pushcfunction(L,object_create); lua_setfield(L,-2,"_object_create");
        lua_pushcfunction(L,object_dispose); lua_setfield(L,-2,"_object_dispose");
        lua_pushcfunction(L, object_registration); lua_setfield(L,-2,"_object_registration");
        lua_pushcfunction(L, object_attach); lua_setfield(L,-2,"_object_attach");
        lua_pushcfunction(L, object_close); lua_setfield(L,-2,"_object_close");
        lua_pushcfunction(L, object_validate); lua_setfield(L,-2,"_object_check");
        lua_pushcfunction(L, current_entity); lua_setfield(L,-2,"_current_entity");
        lua_pushcfunction(L, reject_request); lua_setfield(L, -2, "reject");
        lua_pushcfunction(L, attributes_proposal); lua_setfield(L, -2, "_attributes_proposal");
        lua_pushcfunction(L, attributes_apply); lua_setfield(L, -2, "_attributes_apply");
        lua_pushcfunction(L, attributes_define); lua_setfield(L, -2, "_attributes_define");
        lua_pushcfunction(L, attributes_open); lua_setfield(L, -2, "_attributes_open");
        lua_pushcfunction(L, attributes_close); lua_setfield(L, -2, "_attributes_close");
        luaL_newmetatable(L,"fsnet.field");
        lua_pushcfunction(L,field_index); lua_setfield(L,-2,"__index");
        lua_pushcfunction(L,field_newindex); lua_setfield(L,-2,"__newindex");
        lua_pushcfunction(L,field_length); lua_setfield(L,-2,"__len");
        lua_pushcfunction(L,field_pairs); lua_setfield(L,-2,"__pairs");
        lua_pushcfunction(L,field_gc); lua_setfield(L,-2,"__gc");
        lua_pushboolean(L,false); lua_setfield(L,-2,"__metatable"); lua_pop(L,1);
        luaL_newmetatable(L, "fsnet.attributes");
        lua_pushcfunction(L, attributes_gc); lua_setfield(L, -2, "__gc");
        lua_pushcfunction(L, attributes_index); lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, attributes_newindex); lua_setfield(L, -2, "__newindex");
        lua_pushboolean(L, false); lua_setfield(L, -2, "__metatable"); lua_pop(L, 1);
        lua_pushcfunction(L, rpc); lua_setfield(L, -2, "rpc");
        lua_pushcfunction(L, connect); lua_setfield(L, -2, "connect");
        lua_pushcfunction(L, request); lua_setfield(L, -2, "await");
        lua_newtable(L);
        int status = 0;
        for (const char* name : {"ok", "not_found", "overloaded", "timeout", "disconnected", "rejected", "internal"}) {
            lua_pushinteger(L, status++); lua_setfield(L, -2, name);
        }
        lua_setfield(L, -2, "status");
        lua_setglobal(L, "runtime");
        const char* wrapper = "runtime.request = function(...) local data, status = runtime.await(...); "
                              "if status ~= 0 then error('RPC failed: ' .. status, 2) end; return data end";
        if (luaL_loadstring(L, wrapper) != LUA_OK) return lua_error(L);
        lua_call(L, 0, 0);
        const char* entities = R"lua(
            local register_rpc = runtime.rpc
            runtime.rpc = function(route, handler, kind, initial, policy)
                if type(handler) == 'table' then
                    local definition = handler
                    assert(kind == nil and initial == nil and policy == nil and definition.access == nil, 'invalid stateless RPC definition')
                    assert(type(definition.handler) == 'function' and type(definition.callers) == 'table' and #definition.callers > 0, 'explicit RPC callers and handler required')
                    policy = {}
                    for _, caller in ipairs(definition.callers) do
                        assert(caller == 'client' or caller == 'server', 'invalid RPC caller')
                        policy[caller] = true
                    end
                    handler = function(data, ctx) return definition.handler(ctx, data) end
                end
                return register_rpc(route, handler, kind, initial, policy)
            end
            runtime.entity_rpc = function(kind, method, handler)
                assert(type(method) == 'string' and #method > 0 and #method <= 127 and not method:find('[%.%z]'), 'invalid entity method')
                runtime.rpc(kind .. '.' .. method, handler, kind)
            end
            runtime.entity = function(kind, definition)
                assert(type(definition) == 'table' and type(definition.methods) == 'table', 'entity definition required')
                assert(not (definition.attrs and definition.initial), 'attribute entities cannot declare opaque initial state')
                assert(definition.owner == nil or (definition.attrs and definition.owner == 'id'), 'unsupported entity owner declaration')
                local initial = definition.attrs and (definition.model and runtime._model_publish(kind) or runtime._attributes_define(kind, definition.attrs, definition.owner)) or definition.initial or ''
                local register = function(method, handler)
                    local policy
                    assert(not definition.attrs or type(handler) == 'table', 'attribute entity methods require explicit callers')
                    if type(handler) == 'table' then
                        assert(handler.access == nil or handler.access == 'owner', 'invalid entity access')
                        assert(type(handler.callers) == 'table' and #handler.callers > 0, 'explicit RPC callers required')
                        policy = {owner_only = handler.access == 'owner'}
                        for _, caller in ipairs(handler.callers) do
                            assert(caller == 'client' or caller == 'server', 'invalid RPC caller')
                            policy[caller] = true
                        end
                        handler = handler.handler
                    end
                    assert(type(method) == 'string' and #method > 0 and #method <= 127 and not method:find('[%.%z]'), 'invalid entity method')
                    assert(type(handler) == 'function', 'entity handler required')
                    runtime.rpc(kind .. '.' .. method, function(id, data, ctx)
                        assert(data:sub(1, 4) == 'FSS1', 'invalid state boundary')
                        local size, offset = string.unpack('>I4', data, 5)
                        assert(#data >= offset + size - 1, 'truncated state')
                        local state = data:sub(offset, offset + size - 1)
                        local next_state, response
                        if definition.attrs then
                            local self = runtime._attributes_open(kind, state)
                            response = handler(self, ctx, data:sub(offset + size))
                            next_state = runtime._attributes_close(self)
                        else next_state, response = handler(state, id, data:sub(offset + size), ctx) end
                        assert(type(next_state) == 'string' and type(response) == 'string', 'return state and response bytes')
                        return 'FSS1' .. string.pack('>I4', #next_state) .. next_state .. response
                    end, kind, initial, policy)
                end
                for method, handler in pairs(definition.methods) do
                    assert(method:sub(1, 1) ~= '$', 'reserved entity method')
                    register(method, handler)
                end
                if definition.attrs then
                    assert(definition.validate_write == nil or type(definition.validate_write) == 'function', 'invalid property validator')
                    register('$write', {callers={'client'}, access='owner', handler=function(self, ctx, payload)
                        local proposal = runtime._attributes_proposal(self, payload)
                        if definition.validate_write and definition.validate_write(self, ctx, proposal) ~= true then runtime.reject() end
                        runtime._attributes_apply(self, payload)
                        return ''
                    end})
                end
                if definition.tick then register('$tick', definition.attrs and {callers={'server'}, handler=definition.tick} or definition.tick) end
            end
            runtime.entity_call = function(kind, id, method, payload, timeout, request_id)
                return runtime.entity_request(0, kind, id, method, payload, timeout, request_id)
            end
            runtime.entity_request = function(peer, kind, id, method, payload, timeout, request_id)
                assert(type(id) == 'string' and #id > 0 and #id <= 256, 'invalid entity ID')
                local envelope
                if request_id then
                    assert(type(request_id) == 'string' and #request_id > 0 and #request_id <= 128, 'invalid request ID')
                    envelope = 'FSE2' .. string.pack('>I2', #id) .. id .. string.pack('>I2', #request_id) .. request_id .. payload
                else envelope = 'FSE1' .. string.pack('>I2', #id) .. id .. payload end
                return runtime.request(peer, kind .. '.' .. method, envelope, timeout)
            end
        )lua";
        if (luaL_loadstring(L, entities) != LUA_OK) return lua_error(L);
        lua_call(L, 0, 0);
        if (luaL_loadbufferx(L, detail::lua_object_api, sizeof(detail::lua_object_api)-1, "@fsnet/entity.lua", "t") != LUA_OK) return lua_error(L);
        lua_call(L,0,0); return 0;
    }
    static int compile(lua_State* L) {
        auto& owner = self(L);
        if (luaL_loadbufferx(L, owner.source.data(), owner.source.size(), owner.chunk_name.c_str(), "t") != LUA_OK)
            return lua_error(L);
        owner.compiled = luaL_ref(L, LUA_REGISTRYINDEX); return 0;
    }
    static int prepare(lua_State* L) {
        auto& task = *self(L).preparing;
        task.thread = lua_newthread(L);
        task.reference = luaL_ref(L, LUA_REGISTRYINDEX);
        lua_rawgeti(L, LUA_REGISTRYINDEX, task.function);
        if (!task.mailbox.empty()) lua_pushlstring(L, task.entity_id.data(), task.entity_id.size());
        if (!task.startup) lua_pushlstring(L, task.data.data(), task.data.size());
        if (task.contextual) {
            lua_createtable(L, 0, 5);
            lua_pushlstring(L,task.entity_id.data(),task.entity_id.size()); lua_setfield(L,-2,"entity_id");
            lua_pushlstring(L,task.entity_type.data(),task.entity_type.size()); lua_setfield(L,-2,"entity_type");
            lua_pushlstring(L, task.identity.subject.data(), task.identity.subject.size()); lua_setfield(L, -2, "subject");
            lua_pushstring(L, task.identity.caller == Caller::server ? "server" : "client"); lua_setfield(L, -2, "caller");
            lua_pushinteger(L, static_cast<lua_Integer>(task.identity.peer)); lua_setfield(L, -2, "peer");
        }
        lua_xmove(L, task.thread, task.startup ? 1 : (task.mailbox.empty() ? 2 : 3) + (task.contextual ? 1 : 0)); return 0;
    }
    static int return_values(lua_State* L) {
        auto& owner = self(L);
        auto& task = *owner.preparing;
        if (!lua_checkstack(task.thread, 2)) return luaL_error(L, "coroutine stack exhausted");
        lua_pushlstring(L, owner.returning->data.data(), owner.returning->data.size());
        lua_pushinteger(L, static_cast<int>(owner.returning->status));
        lua_xmove(L, task.thread, 2); return 0;
    }
    int protect(lua_CFunction function) {
        lua_pushcfunction(state, function);
        return lua_pcall(state, 0, 0, 0);
    }
    std::string message(lua_State* L) const {
        if (lua_type(L, -1) != LUA_TSTRING) return "non-string Lua error";
        std::size_t size; const char* data = lua_tolstring(L, -1, &size);
        return std::string(data, std::min<std::size_t>(size, 4096));
    }
    Impl(std::shared_ptr<ScriptHost> parent, ScriptLimits settings) : host(std::move(parent)), limits(settings) {
        if (!host || limits.memory_bytes < 256 * 1024 || !limits.source_bytes || !limits.queued_requests || !limits.queued_bytes ||
            !limits.active_coroutines || !limits.registered_routes || limits.instructions_per_resume < 1000 ||
            limits.task_timeout.count() <= 0 || limits.task_timeout > std::chrono::seconds(30))
            throw std::invalid_argument("invalid script host or limits");
        state = lua_newstate(allocate, this);
        if (!state) throw std::runtime_error("Lua VM allocation failed");
        *static_cast<Impl**>(lua_getextraspace(state)) = this;
        if (protect(initialize) != LUA_OK) {
            auto detail = message(state); lua_close(state); state = nullptr;
            throw std::runtime_error("Lua initialization: " + detail);
        }
    }
    ~Impl() { if (state) lua_close(state); }
    int resume(Task& task, int arguments, int& results) {
        exhausted = false; budget = limits.instructions_per_resume;
        lua_sethook(task.thread, hook, LUA_MASKCOUNT, 1000);
        return lua_resume(task.thread, state, arguments, &results);
    }
    void finish(std::uint64_t id, Result result) {
        auto it = tasks.find(id);
        if (it == tasks.end()) return;
        auto task = std::move(it->second); tasks.erase(it);
        threads.erase(task.thread);
        if (task.reference != LUA_NOREF) luaL_unref(state, LUA_REGISTRYINDEX, task.reference);
        if(task.background && task.function!=LUA_NOREF) luaL_unref(state,LUA_REGISTRYINDEX,task.function);
        if (!task.mailbox.empty()) busy.erase(task.mailbox);
        task.reply(std::move(result));
    }
    void advance(std::uint64_t id, int arguments) {
        auto it = tasks.find(id);
        if (it == tasks.end()) return;
        auto& task = it->second;
        int results = 0;
        int status = resume(task, arguments, results);
        if (task.rejected) { error = "script rejected RPC"; return finish(id, {Status::rejected, {}}); }
        if (exhausted) { error = "Lua instruction budget exceeded"; return finish(id, {Status::overloaded, {}}); }
        if (status == LUA_OK) {
            if (results != 1 || lua_type(task.thread, -1) != LUA_TSTRING) {
                error = "RPC handler must return one binary string"; return finish(id, {Status::internal, {}});
            }
            std::size_t size; const char* data = lua_tolstring(task.thread, -1, &size);
            if (size > limits.queued_bytes) return finish(id, {Status::overloaded, {}});
            return finish(id, {Status::ok, std::string(data, size)});
        }
        if (status != LUA_YIELD) {
            error = message(task.thread);
            return finish(id, {status == LUA_ERRMEM ? Status::overloaded : Status::internal, {}});
        }
        if (!task.await) { error = "unmanaged coroutine yield"; return finish(id, {Status::internal, {}}); }
        auto request = std::move(*task.await); task.await.reset(); task.waiting = true;
        if(task.background) task.deadline=Clock::now()+limits.task_timeout;
        const auto generation = ++task.generation;
        if(request.kind==Await::Kind::sleep) { task.wake=Clock::now()+request.timeout; return; }
        try {
            auto completion = [weak = weak_from_this(), id, generation](Result result) {
                    auto owner = weak.lock();
                    if (!owner || owner->stopped) return;
                    auto current = owner->tasks.find(id);
                    if (current == owner->tasks.end() || !current->second.waiting ||
                        current->second.generation != generation || current->second.completion_queued) return;
                    if (result.data.size() > owner->limits.queued_bytes - owner->completed_bytes)
                        result = {Status::overloaded, {}};
                    auto size = result.data.size();
                    owner->resumes.push_back({id, std::move(result)});
                    owner->completed_bytes += size;
                    current->second.completion_queued = true;
                };
            if(request.kind==Await::Kind::watch) {
                host->watch_attributes(std::move(request.route),std::move(request.data),
                    [weak=weak_from_this(),weak_host=std::weak_ptr(host),id,generation,completion=std::move(completion)](Status status,std::uint64_t subscription) mutable {
                        auto owner=weak.lock();
                        if(!owner || owner->stopped || !owner->tasks.contains(id) || owner->tasks.at(id).generation!=generation) {
                            if(subscription) if(auto parent=weak_host.lock()) parent->unwatch_attributes(subscription);
                            return;
                        }
                        std::string result;
                        if(status==Status::ok) for(int shift=56;shift>=0;shift-=8) result+=static_cast<char>(subscription>>shift);
                        completion({status,std::move(result)});
                    },request.timeout);
            } else if(request.kind==Await::Kind::write) host->write_attributes(request.subscription,std::move(request.values),std::move(completion),std::move(request.options),request.timeout);
            else if(request.kind==Await::Kind::spawn) host->spawn_entity(std::move(request.route),std::move(request.data),std::move(request.values),std::move(completion));
            else if(request.kind==Await::Kind::visibility) host->entity_visibility(request.peer,std::move(request.route),std::move(request.data),request.action,std::move(completion));
            else if (request.peer) host->call(request.peer, std::move(request.route), std::move(request.data), request.timeout, std::move(completion));
            else host->call_entity(std::move(request.route), std::move(request.data), request.timeout, std::move(completion));
        } catch (const std::exception& exception) { error = exception.what(); finish(id, {Status::internal, {}}); }
    }
    void stop() {
        if (std::exchange(stopped, true)) return;
        for (auto& route : bound) host->unbind(route);
        bound.clear();
        for (const auto& type : defined_schemas) host->forget_schema(type);
        defined_schemas.clear();
        auto queued = std::move(queue); queue.clear(); queued_bytes = 0;
        for (auto& task : queued) {
            try { task.reply({Status::disconnected, {}}); } catch (...) { /* complete remaining shutdown work */ }
        }
        while (!tasks.empty()) {
            try { finish(tasks.begin()->first, {Status::disconnected, {}}); } catch (...) { /* task already removed */ }
        }
        resumes.clear(); completed_bytes = 0; routes.clear();
        if (state) { lua_close(state); state = nullptr; }
    }
};
LuaEngine::LuaEngine(std::shared_ptr<ScriptHost> host, ScriptLimits limits)
  : impl_(std::make_shared<Impl>(std::move(host), limits)) {}
LuaEngine::~LuaEngine() { stop(); }
void LuaEngine::load(std::string_view source, const std::string& name) {
    auto& owner = *impl_;
    if (owner.stopped || owner.loaded || owner.loading) throw std::logic_error("script engine is one-shot");
    if (source.size() > owner.limits.source_bytes) throw std::length_error("script source too large");
    owner.loading = true; owner.source = source; owner.chunk_name = name;
    try {
        if (owner.protect(Impl::compile) != LUA_OK) throw std::runtime_error(owner.message(owner.state));
        Impl::Task startup; startup.startup = true; startup.function = owner.compiled;
        owner.preparing = &startup;
        int prepared = owner.protect(Impl::prepare);
        if (prepared != LUA_OK) throw std::runtime_error(owner.message(owner.state));
        int results = 0; int status = owner.resume(startup, 0, results);
        if (owner.exhausted) throw std::runtime_error("Lua load instruction budget exceeded");
        if (status != LUA_OK) throw std::runtime_error(owner.message(startup.thread));
        luaL_unref(owner.state, LUA_REGISTRYINDEX, startup.reference);
        luaL_unref(owner.state, LUA_REGISTRYINDEX, owner.compiled); owner.compiled = LUA_NOREF;
        owner.preparing = nullptr; owner.loading = false;
        for (auto& [route, reference] : owner.routes) {
            (void)reference;
            if (owner.host->has_route(route)) throw std::runtime_error("host route already exists: " + route);
        }
        for (const auto& [route, kind] : owner.entity_routes) {
            bool managed = owner.stateful_routes.contains(route);
            for (const auto& [other, type] : owner.entity_routes)
                if (kind == type && managed != owner.stateful_routes.contains(other))
                    throw std::runtime_error("cannot mix persistent and unmanaged methods for one entity type");
        }
        owner.defined_schemas.reserve(owner.schemas.size());
        for (const auto& [type, schema] : owner.schemas) {
            if(!owner.server_schemas.contains(type)) continue;
            owner.defined_schemas.push_back(type);
            try { owner.host->define_schema(type, schema); }
            catch (...) { owner.defined_schemas.pop_back(); throw; }
        }
        owner.bound.reserve(owner.routes.size());
        for (auto& [route, reference] : owner.routes) {
            owner.bound.push_back(route);
            auto entity = owner.entity_routes.find(route);
            std::string kind = entity == owner.entity_routes.end() ? "" : entity->second;
            const bool contextual = owner.policies.contains(route);
            ContextHandler handler = [weak = std::weak_ptr(impl_), reference, kind, contextual](const CallIdentity& identity, std::string data, Reply reply) {
                auto self = weak.lock();
                if (!self || self->stopped || !self->loaded) return reply({Status::disconnected, {}});
                if (self->queue.size() >= self->limits.queued_requests || data.size() > self->limits.queued_bytes - self->queued_bytes)
                    return reply({Status::overloaded, {}});
                Impl::Task task; task.entity_type = kind; task.identity = identity; task.contextual = contextual;
                if (!kind.empty()) {
                    if (data.size() < 6 || data.compare(0, 4, "FSE1") != 0) return reply({Status::rejected, {}});
                    const auto length = (static_cast<unsigned char>(data[4]) << 8) | static_cast<unsigned char>(data[5]);
                    if (!length || length > 256 || data.size() < 6u + length) return reply({Status::rejected, {}});
                    task.entity_id = data.substr(6, length);
                    task.mailbox = kind + '\0' + task.entity_id;
                    data.erase(0, 6 + length);
                }
                task.function = reference; task.data = std::move(data); task.reply = std::move(reply);
                task.deadline = Impl::Clock::now() + self->limits.task_timeout;
                auto bytes = task.data.size();
                self->queue.push_back(std::move(task));
                self->queued_bytes += bytes;
            };
            auto managed = owner.stateful_routes.find(route);
            if (contextual) {
                auto policy = owner.policies.at(route);
                if (managed != owner.stateful_routes.end()) owner.host->bind_stateful_context(route, kind, managed->second, policy, std::move(handler));
                else owner.host->bind_context(route, policy, std::move(handler));
            } else {
                Handler legacy = [handler = std::move(handler)](std::string data, Reply reply) { handler({}, std::move(data), std::move(reply)); };
                if (managed != owner.stateful_routes.end()) owner.host->bind_stateful(route, kind, managed->second, std::move(legacy));
                else owner.host->bind(route, std::move(legacy));
            }
        }
        owner.loaded = true; owner.source = {};
        for(auto function:owner.starts) {
            Impl::Task task; task.function=function; task.background=true;
            task.deadline=Impl::Clock::now()+owner.limits.task_timeout;
            task.reply=[weak=std::weak_ptr(impl_)](Result result) {
                if(result.status!=Status::ok) if(auto self=weak.lock()) self->error="background task failed: "+std::to_string(static_cast<int>(result.status))+" "+self->error;
            };
            owner.queue.push_back(std::move(task));
        }
        owner.starts.clear();
    } catch (...) { owner.error = "script load failed"; owner.stop(); throw; }
}
std::size_t LuaEngine::poll(std::size_t work_limit) {
    auto keep_alive = impl_;
    auto& owner = *keep_alive;
    if (owner.stopped || !owner.loaded) return 0;
    if (owner.polling) throw std::logic_error("recursive script poll");
    owner.polling = true;
    struct Reset { bool& flag; ~Reset() { flag = false; } } reset{owner.polling};
    std::size_t work = 0;
    const auto now = Impl::Clock::now();
    for(auto& [id,task]:owner.tasks) if(task.wake && *task.wake<=now) {
        task.wake.reset(); task.completion_queued=true; owner.resumes.push_back({id,{Status::ok,{}}});
    }
    while (!owner.queue.empty() && owner.queue.front().deadline <= now) {
        auto task = std::move(owner.queue.front()); owner.queue.pop_front(); owner.queued_bytes -= task.data.size();
        task.reply({Status::timeout, {}});
    }
    std::vector<std::uint64_t> expired;
    for (auto& [id, task] : owner.tasks) if (task.deadline <= now) expired.push_back(id);
    for (auto id : expired) owner.finish(id, {Status::timeout, {}});
    while (!owner.stopped && !owner.stop_requested && work < work_limit) {
        if (!owner.resumes.empty()) {
            auto completion = std::move(owner.resumes.front()); owner.resumes.pop_front();
            owner.completed_bytes -= completion.result.data.size();
            auto it = owner.tasks.find(completion.id);
            if (it == owner.tasks.end() || !it->second.waiting) continue;
            auto& task = it->second; task.waiting = false; task.completion_queued = false;
            owner.preparing = &task; owner.returning = &completion.result;
            auto status = owner.protect(Impl::return_values);
            owner.preparing = nullptr; owner.returning = nullptr;
            if (status != LUA_OK) {
                owner.error = owner.message(owner.state); lua_pop(owner.state, 1);
                owner.finish(completion.id, {Status::overloaded, {}});
            } else owner.advance(completion.id, 2);
        } else if (!owner.queue.empty() && owner.tasks.size() < owner.limits.active_coroutines) {
            auto eligible = std::find_if(owner.queue.begin(), owner.queue.end(), [&](const auto& candidate) {
                return candidate.mailbox.empty() || !owner.busy.contains(candidate.mailbox);
            });
            if (eligible == owner.queue.end()) break;
            auto task = std::move(*eligible); owner.queue.erase(eligible);
            owner.queued_bytes -= task.data.size();
            owner.preparing = &task;
            auto status = owner.protect(Impl::prepare); owner.preparing = nullptr;
            if (status != LUA_OK) {
                owner.error = owner.message(owner.state); lua_pop(owner.state, 1);
                if (task.reference != LUA_NOREF) luaL_unref(owner.state, LUA_REGISTRYINDEX, task.reference);
                task.reply({Status::overloaded, {}});
            } else {
                auto id = ++owner.serial; owner.threads.emplace(task.thread, id);
                const auto arguments = (task.mailbox.empty() ? 1 : 2) + (task.contextual ? 1 : 0);
                if (!task.mailbox.empty()) owner.busy.insert(task.mailbox);
                task.data.clear(); owner.tasks.emplace(id, std::move(task)); owner.advance(id, arguments);
            }
        } else break;
        ++work;
    }
    owner.polling = false;
    if (owner.stop_requested) owner.stop();
    return work;
}
void LuaEngine::stop() {
    if (impl_->polling) impl_->stop_requested = true;
    else impl_->stop();
}
std::size_t LuaEngine::active() const { return impl_->tasks.size() + impl_->queue.size(); }
std::size_t LuaEngine::memory_used() const { return impl_->used; }
std::string LuaEngine::last_error() const { return impl_->error; }
}

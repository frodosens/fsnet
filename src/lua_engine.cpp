#include "fsnet/lua_engine.hpp"
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include <algorithm>
#include <cstdlib>
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
    };
    struct Task {
        lua_State* thread = nullptr;
        int reference = LUA_NOREF, function = LUA_NOREF;
        std::string data, entity_id, mailbox;
        Reply reply;
        std::optional<Await> await;
        bool waiting = false, startup = false, completion_queued = false;
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
            lua_pushvalue(L, 2);
            int reference = luaL_ref(L, LUA_REGISTRYINDEX);
            try {
                if (!entity_type.empty()) owner.entity_routes.emplace(route, std::move(entity_type));
                if (initial_state) owner.stateful_routes.emplace(route, std::move(*initial_state));
                owner.routes.emplace(route, reference);
            } catch (const std::exception&) {
                owner.entity_routes.erase(route); owner.stateful_routes.erase(route);
                luaL_unref(L, LUA_REGISTRYINDEX, reference); throw;
            }
            return 0;
        } catch (const std::exception& error) { return luaL_error(L, "%s", error.what()); }
    }
    static int connect(lua_State* L) {
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
        auto peer = luaL_checkinteger(L, 1);
        luaL_checktype(L, 2, LUA_TSTRING); luaL_checktype(L, 3, LUA_TSTRING);
        auto timeout = luaL_optinteger(L, 4, 1000);
        if (peer <= 0 || timeout <= 0 || timeout > 30000) return luaL_error(L, "invalid peer or deadline");
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
            owner.tasks.at(it->second).await = Await{static_cast<std::uint64_t>(peer), std::string(route, route_size),
                std::string(data, size), std::chrono::milliseconds(timeout)};
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
            runtime.entity_rpc = function(kind, method, handler)
                assert(type(method) == 'string' and #method > 0 and #method <= 127 and not method:find('[%.%z]'), 'invalid entity method')
                runtime.rpc(kind .. '.' .. method, handler, kind)
            end
            runtime.entity = function(kind, definition)
                assert(type(definition) == 'table' and type(definition.methods) == 'table', 'entity definition required')
                local initial = definition.initial or ''
                local register = function(method, handler)
                    assert(type(method) == 'string' and #method > 0 and #method <= 127 and not method:find('[%.%z]'), 'invalid entity method')
                    assert(type(handler) == 'function', 'entity handler required')
                    runtime.rpc(kind .. '.' .. method, function(id, data)
                        assert(data:sub(1, 4) == 'FSS1', 'invalid state boundary')
                        local size, offset = string.unpack('>I4', data, 5)
                        assert(#data >= offset + size - 1, 'truncated state')
                        local state = data:sub(offset, offset + size - 1)
                        local next_state, response = handler(state, id, data:sub(offset + size))
                        assert(type(next_state) == 'string' and type(response) == 'string', 'return state and response bytes')
                        return 'FSS1' .. string.pack('>I4', #next_state) .. next_state .. response
                    end, kind, initial)
                end
                for method, handler in pairs(definition.methods) do
                    assert(method:sub(1, 1) ~= '$', 'reserved entity method')
                    register(method, handler)
                end
                if definition.tick then register('$tick', definition.tick) end
            end
            runtime.entity_request = function(peer, kind, id, method, payload, timeout)
                assert(type(id) == 'string' and #id > 0 and #id <= 256, 'invalid entity ID')
                return runtime.request(peer, kind .. '.' .. method, 'FSE1' .. string.pack('>I2', #id) .. id .. payload, timeout)
            end
        )lua";
        if (luaL_loadstring(L, entities) != LUA_OK) return lua_error(L);
        lua_call(L, 0, 0); return 0;
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
        lua_xmove(L, task.thread, task.startup ? 1 : (task.mailbox.empty() ? 2 : 3)); return 0;
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
        if (!task.mailbox.empty()) busy.erase(task.mailbox);
        task.reply(std::move(result));
    }
    void advance(std::uint64_t id, int arguments) {
        auto it = tasks.find(id);
        if (it == tasks.end()) return;
        auto& task = it->second;
        int results = 0;
        int status = resume(task, arguments, results);
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
        const auto generation = ++task.generation;
        try {
            host->call(request.peer, std::move(request.route), std::move(request.data), request.timeout,
                [weak = weak_from_this(), id, generation](Result result) {
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
                });
        } catch (const std::exception& exception) { error = exception.what(); finish(id, {Status::internal, {}}); }
    }
    void stop() {
        if (std::exchange(stopped, true)) return;
        for (auto& route : bound) host->unbind(route);
        bound.clear();
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
        owner.bound.reserve(owner.routes.size());
        for (auto& [route, reference] : owner.routes) {
            owner.bound.push_back(route);
            auto entity = owner.entity_routes.find(route);
            std::string kind = entity == owner.entity_routes.end() ? "" : entity->second;
            Handler handler = [weak = std::weak_ptr(impl_), reference, kind](std::string data, Reply reply) {
                auto self = weak.lock();
                if (!self || self->stopped || !self->loaded) return reply({Status::disconnected, {}});
                if (self->queue.size() >= self->limits.queued_requests || data.size() > self->limits.queued_bytes - self->queued_bytes)
                    return reply({Status::overloaded, {}});
                Impl::Task task;
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
            if (auto managed = owner.stateful_routes.find(route); managed != owner.stateful_routes.end())
                owner.host->bind_stateful(route, kind, managed->second, std::move(handler));
            else owner.host->bind(route, std::move(handler));
        }
        owner.loaded = true; owner.source = {};
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
                const auto arguments = task.mailbox.empty() ? 1 : 2;
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

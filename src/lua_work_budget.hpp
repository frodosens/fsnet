#pragma once
#include "lua.h"
#include <cstddef>

// Private bridge shared by the pinned VM's instrumented string library and host.
// Lua copies extraspace to new coroutines, so every thread uses the same budget.
struct FsnetLuaWorkBudget {
    void* context;
    void (*charge)(lua_State*, void*, std::size_t);
};
inline void fsnet_lua_pattern_step(lua_State* state, std::size_t units = 1) {
    auto* budget = *static_cast<FsnetLuaWorkBudget**>(lua_getextraspace(state));
    budget->charge(state, budget->context, units);
}

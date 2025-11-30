#pragma once

#include <lua.hpp>

// Register all Lua functions with the given state
void register_lua_bindings(lua_State *L);

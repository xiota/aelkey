// Compatibility helpers for Lua 5.1 vs Lua 5.2+ / 5.4

#pragma once

#include <lua.hpp>

#if LUA_VERSION_NUM == 501 && !defined(LUAJIT_VERSION)
/* Compatibility definitions for plain Lua 5.1 */
#  define luaL_newlib(L, funcs) (lua_newtable(L), luaL_register(L, nullptr, funcs))
#endif

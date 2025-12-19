// Compatibility helpers for Lua 5.1 vs Lua 5.2+ / 5.4

#pragma once

#include <iostream>

#include <lua.hpp>

#if LUA_VERSION_NUM == 501 && !defined(LUAJIT_VERSION)
/* Compatibility definitions for plain Lua 5.1 */
#  define luaL_newlib(L, funcs) (lua_newtable(L), luaL_register(L, nullptr, funcs))
#endif

#if LUA_VERSION_NUM < 504
inline void lua_warning(lua_State *L, const char *msg, int tocont) {
  if (msg) {
    std::cerr << "Lua warning: " << msg;
    if (!tocont) {
      std::cerr << std::endl;  // terminate warning line
    }
  }
}
#endif

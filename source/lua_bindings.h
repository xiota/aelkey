#pragma once

#include <string>

#include <lua.hpp>

struct TickCallback {
  bool is_function = false;
  int ref = LUA_NOREF;
  std::string name;
};

// Register all Lua functions with the given state
void register_lua_bindings(lua_State *L);

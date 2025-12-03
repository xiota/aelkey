#pragma once

#include <filesystem>
#include <string>

#include <lua.hpp>

struct TickCallback {
  bool is_function = false;
  int ref = LUA_NOREF;
  std::string name;
};

int lua_run(lua_State *L);
int lua_emit(lua_State *L);
int lua_syn_report(lua_State *L);
int lua_tick(lua_State *L);

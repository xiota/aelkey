#pragma once

#include <lua.hpp>

int lua_open_device(lua_State *L);
int lua_close_device(lua_State *L);
int lua_get_device_info(lua_State *L);

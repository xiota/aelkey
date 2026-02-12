#pragma once

#include <cstdint>
#include <string>

#include <lua.hpp>

uint64_t util_now(const std::string &unit = "ms");

extern "C" int luaopen_aelkey_util(lua_State *L);

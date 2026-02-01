#pragma once

#include <string>

#include <sol/sol.hpp>

sol::object evdev_open(sol::this_state ts, sol::optional<std::string> dev_id_opt);

extern "C" int luaopen_aelkey_evdev(lua_State *L);

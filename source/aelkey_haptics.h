#pragma once

#include <sol/sol.hpp>

static constexpr const char *HAPTICS_SOURCE_CUSTOM = "_aelkey_haptics_custom_";
static constexpr const char *HAPTICS_SOURCE_ONESHOT = "_aelkey_haptics_oneshot_";

extern "C" int luaopen_aelkey_haptics(lua_State *L);

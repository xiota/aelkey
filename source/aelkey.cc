#include <lua.hpp>

#include "aelkey_bit.h"
#include "aelkey_core.h"
#include "aelkey_device.h"
#include "aelkey_loop.h"
#include "aelkey_util.h"
#include "luacompat.h"

extern "C" int luaopen_aelkey(lua_State *L) {
  // clang-format off
  static const luaL_Reg funcs[] = {
    {"emit", lua_emit},
    {"run", lua_run},
    {"stop", lua_stop},
    {"syn_report", lua_syn_report},
    {"tick", lua_tick},

    {"open_device", lua_open_device},
    {"close_device", lua_close_device},
    {"get_device_info", lua_get_device_info},
    {nullptr, nullptr}
  };
  // clang-format on

  luaL_newlib(L, funcs);

  // bit submodule
  luaopen_aelkey_bit(L);
  lua_setfield(L, -2, "bit");

  // util submodule
  luaopen_aelkey_util(L);
  lua_setfield(L, -2, "util");

  return 1;
}

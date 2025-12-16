#include <lua.hpp>

#include "aelkey_bit.h"
#include "aelkey_core.h"
#include "aelkey_daemon.h"
#include "aelkey_device.h"
#include "aelkey_hid.h"
#include "aelkey_loop.h"
#include "aelkey_usb.h"
#include "aelkey_util.h"
#include "lua_scripts.h"
#include "luacompat.h"
#include "tick_scheduler.h"

extern "C" int luaopen_aelkey(lua_State *L) {
  // clang-format off
  static const luaL_Reg funcs[] = {
    {"emit", lua_emit},
    {"start", lua_start},
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

  // click submodule
  if (luaL_loadstring(L, aelkey_click_script) == LUA_OK) {
    lua_call(L, 0, 1);
    lua_setfield(L, -2, "click");  // aelkey.click = module
  } else {
    lua_error(L);
  }

  // bit submodule
  luaopen_aelkey_bit(L);
  lua_setfield(L, -2, "bit");

  // daemon submodule
  luaopen_aelkey_daemon(L);
  lua_setfield(L, -2, "daemon");

  // hid submodule
  luaopen_aelkey_hid(L);
  lua_setfield(L, -2, "hid");

  // usb submodule
  luaopen_aelkey_usb(L);
  lua_setfield(L, -2, "usb");

  // util submodule
  luaopen_aelkey_util(L);
  lua_setfield(L, -2, "util");

  return 1;
}

#include "aelkey_hid.h"

#include <libusb-1.0/libusb.h>
#include <lua.hpp>

#include "aelkey_state.h"
#include "luacompat.h"

static int lua_bulk_transfer(lua_State *L) {
  return 0;
}

static int lua_control_transfer(lua_State *L) {
  return 0;
}

static int lua_interrupt_transfer(lua_State *L) {
  return 0;
}

static int lua_submit_transfer(lua_State *L) {
  return 0;
}

extern "C" int luaopen_aelkey_usb(lua_State *L) {
  // clang-format off
  static const luaL_Reg funcs[] = {
    {"bulk_transfer", lua_bulk_transfer},
    {"control_transfer", lua_control_transfer},
    {"interrupt_transfer", lua_interrupt_transfer},
    {"submit_transfer", lua_submit_transfer},
    {nullptr, nullptr}
  };
  // clang-format on

  luaL_newlib(L, funcs);

  return 1;
}

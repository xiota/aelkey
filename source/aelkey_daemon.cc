#include "aelkey_daemon.h"

#include <lua.hpp>

#include "lua_scripts.h"
#include "luacompat.h"

static int l_daemon_start(lua_State *L) {
  return 0;
}

static int l_daemon_stop(lua_State *L) {
  return 0;
}

static int l_daemon_watch(lua_State *L) {
  return 0;
}

static int l_daemon_unwatch(lua_State *L) {
  return 0;
}

static int l_daemon_watchlist(lua_State *L) {
  return 0;
}

int luaopen_aelkey_daemon(lua_State *L) {
  // clang-format off
  static const luaL_Reg funcs[] = {
    {"start", l_daemon_start},
    {"stop", l_daemon_stop},
    {"watch", l_daemon_watch},
    {"unwatch", l_daemon_unwatch},
    {"watchlist", l_daemon_watchlist},
    {nullptr, nullptr}
  };
  // clang-format on

  luaL_newlib(L, funcs);

  // lua scripts
  if (luaL_loadstring(L, aelkey_daemon_script) == 0) {
    lua_pushvalue(L, -2);
    lua_call(L, 1, 0);
  } else {
    lua_error(L);
  }

  return 1;
}

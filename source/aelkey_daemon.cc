#include "aelkey_daemon.h"

#include <lua.hpp>

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
  return 1;
}

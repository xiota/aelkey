#include "aelkey_daemon.h"

#include <lua.hpp>

#include "aelkey_state.h"
#include "device_input.h"
#include "lua_scripts.h"
#include "luacompat.h"

static int l_daemon_start(lua_State *L) {
  if (aelkey_state.active_mode == AelkeyState::ActiveMode::LOOP) {
    luaL_error(L, "cannot start event loop while daemon is running");
    return 1;
  } else if (aelkey_state.active_mode == AelkeyState::ActiveMode::DAEMON) {
    lua_warning(L, "event loop is already running");
    return 1;
  }

  aelkey_state.aelkey_set_mode(AelkeyState::ActiveMode::LOOP);

  aelkey_state.aelkey_set_mode(AelkeyState::ActiveMode::NONE);

  lua_pushboolean(L, 1);
  return 1;
}

static int l_daemon_stop(lua_State *L) {
  aelkey_state.daemon_should_stop = true;
  return 0;
}

static int l_daemon_watch(lua_State *L) {
  // 1. Get reference string
  const char *ref = luaL_checkstring(L, 1);

  // 2. Get declarations table
  luaL_checktype(L, 2, LUA_TTABLE);

  std::vector<InputDecl> decls;

  // Iterate over outer table (list of decl tables)
  int len = lua_objlen(L, 2);
  for (int i = 1; i <= len; i++) {
    lua_rawgeti(L, 2, i);
    if (lua_istable(L, -1)) {
      InputDecl decl = parse_input(L, lua_gettop(L));
      decls.push_back(decl);
    }
    lua_pop(L, 1);
  }

  // 3. Store in watch_map
  aelkey_state.watch_map[ref] = decls;

  return 0;
}

static int l_daemon_unwatch(lua_State *L) {
  // 1. Get reference string
  const char *ref = luaL_checkstring(L, 1);

  // 2. Erase from watch_map if present
  auto it = aelkey_state.watch_map.find(ref);
  if (it != aelkey_state.watch_map.end()) {
    aelkey_state.watch_map.erase(it);
  }

  return 0;
}

static int l_daemon_watchlist(lua_State *L) {
  lua_newtable(L);

  int i = 1;
  for (const auto &entry : aelkey_state.watch_map) {
    lua_pushstring(L, entry.first.c_str());
    lua_rawseti(L, -2, i++);
  }

  return 1;
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

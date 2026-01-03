#include "aelkey_daemon.h"

#include <sol/sol.hpp>

#include "aelkey_state.h"
#include "device_input.h"
#include "lua_scripts.h"

// watch(ref, decls)
// No return value
sol::object daemon_watch(sol::this_state ts, const std::string &ref, sol::table decls_tbl) {
  lua_State *L = ts;
  sol::state_view lua(L);

  std::vector<InputDecl> decls;

  int len = decls_tbl.size();
  for (int i = 1; i <= len; i++) {
    sol::object entry = decls_tbl[i];
    if (entry.is<sol::table>()) {
      sol::table t = entry.as<sol::table>();
      InputDecl decl = parse_input(t);
      decls.push_back(decl);
    }
  }

  aelkey_state.watch_map[ref] = decls;
  return sol::make_object(lua, sol::lua_nil);
}

// unwatch(ref)
// No return value
sol::object daemon_unwatch(sol::this_state ts, const std::string &ref) {
  auto it = aelkey_state.watch_map.find(ref);
  if (it != aelkey_state.watch_map.end()) {
    aelkey_state.watch_map.erase(it);
  }
  return sol::make_object(ts, sol::lua_nil);
}

// watchlist()
// Returns array of reference strings
sol::object daemon_watchlist(sol::this_state ts) {
  lua_State *L = ts;
  sol::state_view lua(L);

  sol::table t = lua.create_table();

  int i = 1;
  for (const auto &entry : aelkey_state.watch_map) {
    t[i++] = entry.first;
  }

  return sol::make_object(lua, t);
}

extern "C" int luaopen_aelkey_daemon(lua_State *L) {
  sol::state_view lua(L);

  sol::table mod = lua.create_table();

  mod.set_function("watch", daemon_watch);
  mod.set_function("unwatch", daemon_unwatch);
  mod.set_function("watchlist", daemon_watchlist);

  // Load embedded Lua script
  sol::load_result chunk = lua.load(aelkey_daemon_script);
  if (!chunk.valid()) {
    sol::error err = chunk;
    throw sol::error(
        "aelkey.daemon: failed to load embedded script: " + std::string(err.what())
    );
  }

  // Call the script with the module table as argument
  sol::protected_function_result result = chunk(mod);
  if (!result.valid()) {
    sol::error err = result;
    throw sol::error("aelkey.daemon: script execution failed: " + std::string(err.what()));
  }

  return sol::stack::push(L, mod);
}

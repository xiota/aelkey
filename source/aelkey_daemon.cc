#include "aelkey_daemon.h"

#include <sol/sol.hpp>

#include "aelkey_state.h"
#include "device_input.h"
#include "lua_scripts.h"

// set_callback(cb)
// Returns true on success, false on invalid input
sol::object daemon_set_callback(sol::this_state ts, sol::object cb_obj) {
  sol::state_view lua(ts);
  auto &state = AelkeyState::instance();

  if (cb_obj.is<std::string>()) {
    state.callback_watchlist = cb_obj.as<std::string>();
    return sol::make_object(lua, true);
  }

  if (cb_obj.is<sol::nil_t>()) {
    state.callback_watchlist.clear();
    return sol::make_object(lua, true);
  }

  std::fprintf(stderr, "aelkey.daemon: set_callback expects string or nil\n");
  return sol::make_object(lua, false);
}

// watch(ref, decls)
// Returns number of valid decls added (0 if none)
sol::object daemon_watch(sol::this_state ts, const std::string &ref, sol::table decls_tbl) {
  sol::state_view lua(ts);

  std::vector<InputDecl> valid_decls;

  int len = decls_tbl.size();
  for (int i = 1; i <= len; i++) {
    sol::object entry = decls_tbl[i];
    if (!entry.is<sol::table>()) {
      continue;
    }

    sol::table t = entry.as<sol::table>();
    InputDecl decl = parse_input(t);

    // Only allow udev-visible types
    if (decl.type == "evdev" || decl.type == "hidraw" || decl.type == "libusb") {
      decl.callback_events.clear();
      decl.callback_state.clear();
      valid_decls.push_back(decl);
    }
  }

  // Store only if at least one valid decl exists
  if (!valid_decls.empty()) {
    auto &state = AelkeyState::instance();
    state.watch_map[ref] = valid_decls;
  }

  // Return number of valid decls added
  return sol::make_object(lua, static_cast<int>(valid_decls.size()));
}

// unwatch(ref)
// No return value
sol::object daemon_unwatch(sol::this_state ts, const std::string &ref) {
  auto &state = AelkeyState::instance();
  auto it = state.watch_map.find(ref);
  if (it != state.watch_map.end()) {
    state.watch_map.erase(it);
  }
  return sol::nil;
}

// watchlist()
// Returns array of reference strings
sol::object daemon_watchlist(sol::this_state ts) {
  sol::state_view lua(ts);
  auto &state = AelkeyState::instance();

  sol::table t = lua.create_table();

  int i = 1;
  for (const auto &entry : state.watch_map) {
    t[i++] = entry.first;
  }

  return sol::make_object(lua, t);
}

extern "C" int luaopen_aelkey_daemon(lua_State *L) {
  sol::state_view lua(L);

  sol::table mod = lua.create_table();

  mod.set_function("set_callback", daemon_set_callback);
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

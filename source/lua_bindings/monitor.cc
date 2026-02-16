#include "lua_bindings/monitor.h"

#include <sol/sol.hpp>

#include "aelkey_state.h"
#include "device_declarations.h"
#include "device_parser.h"
#include "lua_scripts.h"
#include "router_watch_list.h"

// set_callback(cb)
// Returns true on success, false on invalid input
sol::object monitor_set_callback(sol::this_state ts, sol::object cb_obj) {
  sol::state_view lua(ts);
  auto &watch = RouterWatchList::instance();

  if (cb_obj.is<std::string>()) {
    watch.set_callback(cb_obj.as<std::string>());
    return sol::make_object(lua, true);
  }

  if (cb_obj.is<sol::nil_t>()) {
    watch.set_callback("");
    return sol::make_object(lua, true);
  }

  std::fprintf(stderr, "monitor: set_callback expects string or nil\n");
  return sol::make_object(lua, false);
}

// watch(ref, decls)
// Returns number of valid decls added (0 if none)
sol::object monitor_watch(sol::this_state ts, const std::string &ref, sol::table decls_tbl) {
  sol::state_view lua(ts);

  std::vector<InputDecl> valid_decls;

  int len = decls_tbl.size();
  for (int i = 1; i <= len; i++) {
    sol::object entry = decls_tbl[i];
    if (!entry.is<sol::table>()) {
      continue;
    }

    sol::table t = entry.as<sol::table>();
    InputDecl decl = DeviceParser::parse_input(t);

    // Only allow udev-visible types
    if (decl.type == "evdev" || decl.type == "hidraw" || decl.type == "libusb") {
      decl.on_event.clear();
      decl.on_state.clear();
      valid_decls.push_back(decl);
    }
  }

  if (!valid_decls.empty()) {
    auto &watch = RouterWatchList::instance();
    for (auto &d : valid_decls) {
      watch.add_watch(ref, d);
    }
  }

  return sol::make_object(lua, static_cast<int>(valid_decls.size()));
}

// unwatch(ref)
// No return value
sol::object monitor_unwatch(sol::this_state ts, const std::string &ref) {
  auto &watch = RouterWatchList::instance();
  watch.erase_watch(ref);
  return sol::nil;
}

// watchlist()
// Returns array of reference strings
sol::object monitor_watchlist(sol::this_state ts) {
  sol::state_view lua(ts);
  auto &watch = RouterWatchList::instance();

  sol::table t = lua.create_table();

  int i = 1;
  for (const auto &key : watch.keys()) {
    t[i++] = key;
  }

  return sol::make_object(lua, t);
}

extern "C" int luaopen_aelkey_monitor(lua_State *L) {
  sol::state_view lua(L);

  sol::table mod = lua.create_table();

  mod.set_function("set_callback", monitor_set_callback);
  mod.set_function("watch", monitor_watch);
  mod.set_function("unwatch", monitor_unwatch);
  mod.set_function("watchlist", monitor_watchlist);

  // Load embedded Lua script
  sol::load_result chunk = lua.load(aelkey_monitor_script);
  if (!chunk.valid()) {
    sol::error err = chunk;
    throw sol::error("monitor: failed to load embedded script: " + std::string(err.what()));
  }

  // Call the script with the module table as argument
  sol::protected_function_result result = chunk(mod);
  if (!result.valid()) {
    sol::error err = result;
    throw sol::error("monitor: script execution failed: " + std::string(err.what()));
  }

  return sol::stack::push(L, mod);
}

#include <sol/sol.hpp>

#include "aelkey_core.h"
#include "aelkey_daemon.h"
#include "aelkey_device.h"
#include "aelkey_gatt.h"
#include "aelkey_hid.h"
#include "aelkey_loop.h"
#include "aelkey_usb.h"
#include "aelkey_util.h"
#include "lua_scripts.h"
#include "tick_scheduler.h"

namespace {

struct ScriptModule {
  const char *name;
  const char *script;
};

struct CModule {
  const char *name;
  int (*open_func)(lua_State *);
};

// clang-format off
constexpr ScriptModule script_modules[] = {
  { "click", aelkey_click_script },
  { "edge", aelkey_edge_script },
  { "log", aelkey_log_script },
};

constexpr CModule c_modules[] = {
  { "daemon", luaopen_aelkey_daemon },
  { "gatt", luaopen_aelkey_gatt },
  { "hid", luaopen_aelkey_hid },
  { "usb", luaopen_aelkey_usb },
  { "util", luaopen_aelkey_util },
};
// clang-format on

sol::table load_aelkey(sol::state_view lua) {
  sol::table mod = lua.create_table();

  // Core functions
  mod.set_function("emit", core_emit);
  mod.set_function("syn_report", core_syn_report);
  mod.set_function("tick", core_tick);

  // Loop control
  mod.set_function("start", loop_start);
  mod.set_function("stop", loop_stop);

  // Device lifecycle
  mod.set_function("open_device", device_open);
  mod.set_function("close_device", device_close);
  mod.set_function("get_device_info", device_get_info);

  // Script modules
  for (auto &sm : script_modules) {
    try {
      sol::table module = lua.script(sm.script);
      mod[sm.name] = module;
    } catch (const sol::error &err) {
      throw sol::error(
          std::string("aelkey: script module '") + sm.name + "' failed: " + err.what()
      );
    }
  }

  // C modules
  for (auto &cm : c_modules) {
    try {
      cm.open_func(lua.lua_state());
      sol::table module = sol::stack::pop<sol::table>(lua.lua_state());
      mod[cm.name] = module;
    } catch (const sol::error &err) {
      throw sol::error(std::string("aelkey: C module '") + cm.name + "' failed: " + err.what());
    } catch (...) {
      throw sol::error(
          std::string("aelkey: C module '") + cm.name + "' failed with unknown error"
      );
    }
  }

  return mod;
}

}  // namespace

extern "C" int luaopen_aelkey(lua_State *L) {
  sol::state_view lua(L);

  try {
    // Block root access
    if (geteuid() == 0) {
      const char *allow = std::getenv("AELKEY_ALLOW_ROOT");
      if (!allow || allow[0] == '\0') {
        throw sol::error("aelkey: do not run as root.");
      }
    }

    // Normal module creation
    sol::table mod = load_aelkey(lua);
    return sol::stack::push(lua, mod);
  } catch (const sol::error &err) {
    // Turn any sol::error into a real Lua error with a message
    return luaL_error(L, "%s", err.what());
  } catch (const std::exception &e) {
    // Catch other std exceptions too, just in case
    return luaL_error(L, "aelkey: C++ exception: %s", e.what());
  } catch (...) {
    // Absolute last-resort safety net
    return luaL_error(L, "aelkey: unknown C++ exception");
  }
}

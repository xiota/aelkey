#include <sol/sol.hpp>

#include "aelkey_state.h"
#include "lua_bindings/audio.h"
#include "lua_bindings/core.h"
#include "lua_bindings/evdev.h"
#include "lua_bindings/gatt.h"
#include "lua_bindings/haptics.h"
#include "lua_bindings/hid.h"
#include "lua_bindings/jack.h"
#include "lua_bindings/loop.h"
#include "lua_bindings/midi.h"
#include "lua_bindings/monitor.h"
#include "lua_bindings/usb.h"
#include "lua_bindings/util.h"
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
  { "filter", aelkey_filter_script },
  { "keyboard", aelkey_keyboard_script },
  { "log", aelkey_log_script },
  { "mouse", aelkey_mouse_script },
  { "sequence", aelkey_sequence_script },
  { "ticker", aelkey_ticker_script },
  { "touchpad", aelkey_touchpad_script },
  { "tracker", aelkey_tracker_script },
};

constexpr CModule c_modules[] = {
  { "audio", luaopen_aelkey_audio },
  { "evdev", luaopen_aelkey_evdev },
  { "gatt", luaopen_aelkey_gatt },
  { "haptics", luaopen_aelkey_haptics },
  { "hid", luaopen_aelkey_hid },
  { "jack", luaopen_aelkey_jack },
  { "midi", luaopen_aelkey_midi },
  { "monitor", luaopen_aelkey_monitor },
  { "usb", luaopen_aelkey_usb },
  { "util", luaopen_aelkey_util },
};
// clang-format on

sol::table load_modules_c(sol::state_view lua, sol::table mod) {
  mod.set_function("start", loop_start);
  mod.set_function("stop", loop_stop);

  mod.set_function("open_device", core_open_device);
  mod.set_function("close_device", core_close_device);
  mod.set_function("get_device_info", core_get_device_info);

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

sol::table load_modules_scripts(sol::state_view lua, sol::table mod) {
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

  return mod;
}

}  // namespace

extern "C" int luaopen_aelkey(lua_State *L) {
  auto &state = AelkeyState::instance();
  state.lua_vm = L;
  sol::state_view lua(L);

  try {
    // Block root access
    if (geteuid() == 0) {
      const char *allow = std::getenv("AELKEY_ALLOW_ROOT");
      if (!allow || allow[0] == '\0') {
        throw sol::error("aelkey: do not run as root.");
      }
    }

    // Register global
    sol::table mod = lua.create_table();
    lua["aelkey"] = mod;

    load_modules_c(lua, mod);
    load_modules_scripts(lua, mod);

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

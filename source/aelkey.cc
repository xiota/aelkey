#include <sol/sol.hpp>

#include "aelkey_bit.h"
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

struct TopFunc {
  const char *name;
  sol::function (*fn)(lua_State *);
};

// clang-format off
constexpr ScriptModule script_modules[] = {
  { "click", aelkey_click_script },
};

constexpr CModule c_modules[] = {
  { "bit", luaopen_aelkey_bit },
  { "daemon", luaopen_aelkey_daemon },
  { "gatt", luaopen_aelkey_gatt },
  { "hid", luaopen_aelkey_hid },
  { "usb", luaopen_aelkey_usb },
  { "util", luaopen_aelkey_util },
};
// clang-format on

}  // namespace

extern "C" int luaopen_aelkey(lua_State *L) {
  sol::state_view lua(L);

  sol::table mod = lua.create_table();

  mod.set_function("emit", core_emit);
  mod.set_function("start", lua_start);
  mod.set_function("stop", lua_stop);
  mod.set_function("syn_report", core_syn_report);
  mod.set_function("tick", core_tick);

  mod.set_function("open_device", lua_open_device);
  mod.set_function("close_device", lua_close_device);
  mod.set_function("get_device_info", lua_get_device_info);

  // script modules
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
      cm.open_func(L);  // pushes table on stack
      sol::table module = sol::stack::pop<sol::table>(L);
      mod[cm.name] = module;
    } catch (const sol::error &err) {
      throw sol::error(std::string("aelkey: C module '") + cm.name + "' failed: " + err.what());
    } catch (...) {
      throw sol::error(
          std::string("aelkey: C module '") + cm.name + "' failed with unknown error"
      );
    }
  }

  return sol::stack::push(L, mod);
}

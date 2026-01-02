#include "aelkey_core.h"

#include <ctime>

#include <libevdev/libevdev-uinput.h>
#include <sol/sol.hpp>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include "aelkey_state.h"
#include "tick_scheduler.h"

// emit{ device=?, type=?, code=?, value=? }
sol::object core_emit(sol::this_state ts, sol::table opts) {
  lua_State *L = ts;
  sol::state_view lua(L);

  // device (optional)
  sol::optional<std::string> dev_id_opt = opts["device"];
  const char *dev_id = dev_id_opt ? dev_id_opt->c_str() : nullptr;

  // type
  int type = 0;
  sol::object type_obj = opts["type"];
  if (type_obj.is<int>()) {
    type = type_obj.as<int>();
  } else if (type_obj.is<std::string>()) {
    std::string tname = type_obj.as<std::string>();
    type = libevdev_event_type_from_name(tname.c_str());
  }

  // code
  int code = 0;
  sol::object code_obj = opts["code"];
  if (code_obj.is<int>()) {
    code = code_obj.as<int>();
  } else if (code_obj.is<std::string>()) {
    std::string cname = code_obj.as<std::string>();
    code = libevdev_event_code_from_name(type, cname.c_str());
  }

  // value (required)
  int value = opts.get<int>("value");

  // device selection logic
  if (!dev_id) {
    if (aelkey_state.uinput_devices.size() == 1) {
      auto it = aelkey_state.uinput_devices.begin();
      libevdev_uinput_write_event(it->second, type, code, value);
    } else {
      throw sol::error("emit requires 'device' when multiple output devices are present");
    }
  } else {
    auto it = aelkey_state.uinput_devices.find(dev_id);
    if (it == aelkey_state.uinput_devices.end()) {
      throw sol::error("Unknown device id: " + std::string(dev_id));
    }
    libevdev_uinput_write_event(it->second, type, code, value);
  }

  return sol::make_object(lua, sol::lua_nil);
}

// syn_report([device])
sol::object core_syn_report(sol::this_state ts, sol::optional<std::string> dev_id_opt) {
  lua_State *L = ts;
  sol::state_view lua(L);

  if (dev_id_opt) {
    const std::string &dev_id = *dev_id_opt;
    auto it = aelkey_state.uinput_devices.find(dev_id);
    if (it == aelkey_state.uinput_devices.end()) {
      throw sol::error("Unknown device id: " + dev_id);
    }
    libevdev_uinput_write_event(it->second, EV_SYN, SYN_REPORT, 0);
  } else {
    for (auto &kv : aelkey_state.uinput_devices) {
      libevdev_uinput_write_event(kv.second, EV_SYN, SYN_REPORT, 0);
    }
  }

  return sol::make_object(lua, sol::lua_nil);
}

// tick(ms, callback)
// callback = string name OR function
sol::object core_tick(sol::this_state ts, int ms, sol::object cb_obj) {
  lua_State *L = ts;
  sol::state_view lua(L);

  // tick(0) with no callback → stop all
  if (ms == 0 && cb_obj.is<sol::nil_t>()) {
    aelkey_state.scheduler->cancel_all();
    return sol::make_object(lua, sol::lua_nil);
  }

  // Parse callback key
  TickCb key{};
  if (cb_obj.is<std::string>()) {
    key.name = cb_obj.as<std::string>();
    key.is_function = false;
  } else if (cb_obj.is<sol::function>()) {
    sol::function fn = cb_obj.as<sol::function>();
    fn.push();  // push function onto stack
    key.ref = luaL_ref(L, LUA_REGISTRYINDEX);
    key.is_function = true;
  } else {
    throw sol::error("tick callback must be string or function");
  }

  // Cancel existing timers for this key
  aelkey_state.scheduler->cancel_matching(key);

  // If ms == 0, we were just canceling
  if (ms == 0) {
    if (key.is_function && key.ref != LUA_NOREF) {
      luaL_unref(L, LUA_REGISTRYINDEX, key.ref);
    }
    return sol::make_object(lua, sol::lua_nil);
  }

  // Schedule new repeating timer
  int fd = aelkey_state.scheduler->schedule(ms, key);
  if (fd < 0) {
    // schedule failed; clean up
    if (key.is_function && key.ref != LUA_NOREF) {
      luaL_unref(L, LUA_REGISTRYINDEX, key.ref);
    }
    return sol::make_object(lua, sol::lua_nil);
  }

  // feature parity: no handle returned
  return sol::make_object(lua, sol::lua_nil);
}

extern "C" int luaopen_aelkey_core(lua_State *L) {
  sol::state_view lua(L);

  sol::table mod = lua.create_table();

  mod.set_function("emit", core_emit);
  mod.set_function("syn_report", core_syn_report);
  mod.set_function("tick", core_tick);

  return sol::stack::push(L, mod);
}

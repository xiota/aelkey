#include "aelkey_core.h"

#include <ctime>

#include <libevdev/libevdev.h>
#include <sol/sol.hpp>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include "aelkey_state.h"
#include "device_out_uinput.h"
#include "tick_scheduler.h"

// emit{ device=?, type=?, code=?, value=? }
sol::object core_emit(sol::this_state ts, sol::table opts) {
  sol::state_view lua(ts);

  // device (required)
  sol::optional<std::string> dev_id_opt = opts["device"];
  if (!dev_id_opt) {
    throw sol::error("emit requires 'device'");
  }
  const std::string &dev_id = *dev_id_opt;

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

  // value
  int value = opts.get<int>("value");

  auto &outmgr = DeviceOutUinput::instance();
  if (!outmgr.get(dev_id)) {
    throw sol::error("Unknown device id: " + dev_id);
  }

  outmgr.send(dev_id, type, code, value);

  return sol::make_object(lua, sol::lua_nil);
}

// syn_report([device])
sol::object core_syn_report(sol::this_state ts, sol::optional<std::string> dev_id_opt) {
  sol::state_view lua(ts);
  auto &outmgr = DeviceOutUinput::instance();

  if (!dev_id_opt) {
    throw sol::error("syn_report requires 'device'");
  }

  const std::string &dev_id = *dev_id_opt;
  if (!outmgr.get(dev_id)) {
    throw sol::error("Unknown device id: " + dev_id);
  }

  outmgr.sync(dev_id);

  return sol::make_object(lua, sol::lua_nil);
}

// tick(ms, callback)
// callback = string name OR function
sol::object core_tick(sol::this_state ts, int ms, sol::object cb_obj) {
  sol::state_view lua(ts);
  auto &scheduler = TickScheduler::instance();

  // tick(0, nil) → cancel all timers
  if (cb_obj.is<sol::nil_t>()) {
    if (ms == 0) {
      scheduler.cancel_all();
    }
    return sol::lua_nil;
  }

  // Parse callback key
  TickCb key{};
  if (cb_obj.is<std::string>()) {
    key.name = cb_obj.as<std::string>();
    key.is_function = false;
  } else if (cb_obj.is<sol::function>()) {
    key.is_function = true;
    key.fn = cb_obj.as<sol::function>();
  }

  // Cancel existing timers for this key
  scheduler.cancel_matching(key);

  // If ms == 0, we were just canceling
  if (ms == 0) {
    return sol::make_object(lua, sol::lua_nil);
  }

  // Schedule new repeating timer
  int fd = scheduler.schedule(ms, key);
  if (fd < 0) {
    return sol::make_object(lua, sol::lua_nil);
  }

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

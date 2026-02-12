#include "lua_bindings/evdev.h"

#include <string>

#include <libevdev/libevdev.h>
#include <sol/sol.hpp>
#include <string>

#include "device_out_uinput.h"

// send{ device=?, type=?, code=?, value=? }
sol::object evdev_send(sol::this_state ts, sol::table opts) {
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

// sync([device])
sol::object evdev_sync(sol::this_state ts, sol::optional<std::string> dev_id_opt) {
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

extern "C" int luaopen_aelkey_evdev(lua_State *L) {
  sol::state_view lua(L);

  sol::table mod = lua.create_table();

  mod.set_function("send", evdev_send);
  mod.set_function("sync", evdev_sync);

  return sol::stack::push(L, mod);
}

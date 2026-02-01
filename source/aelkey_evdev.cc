#include "aelkey_evdev.h"

#include <libevdev/libevdev.h>
#include <sol/sol.hpp>
#include <string>

#include "aelkey_state.h"
#include "device_declarations.h"
#include "device_in_manager.h"
#include "device_out_uinput.h"
#include "dispatcher_udev.h"

// Lua: open([dev_id])
// Ret: boolean
sol::object evdev_open(sol::this_state ts, sol::optional<std::string> dev_id_opt) {
  sol::state_view lua(ts);
  auto &state = AelkeyState::instance();

  // GLOBAL MODE: no argument → open all devices
  if (!dev_id_opt.has_value()) {
    // Parse declarations from Lua
    state.parse_outputs_from_lua(ts);
    state.parse_inputs_from_lua(ts);

    // Create output devices and attach input devices
    state.create_outputs_from_decls();
    state.attach_inputs_from_decls(ts);

    return sol::make_object(lua, true);
  }

  // SINGLE DEVICE MODE
  std::string dev_id = dev_id_opt.value();

  // Parse declarations if not already parsed
  if (state.input_decls.empty() && state.output_decls.empty()) {
    state.parse_outputs_from_lua(ts);
    state.parse_inputs_from_lua(ts);
    state.create_outputs_from_decls();
  }

  // Attach only the requested device
  bool ok = false;
  for (auto &decl : state.input_decls) {
    if (decl.id != dev_id) {
      continue;
    }

    std::string devnode;
    if (!DeviceInManager::instance().match(decl, devnode)) {
      continue;
    }

    if (DeviceInManager::instance().attach(devnode, decl)) {
      decl.devnode = devnode;
      DispatcherUdev::instance().notify_state_change(decl, "add");
      ok = true;
    }
    break;
  }

  return sol::make_object(lua, ok);
}

// Lua: close([dev_id])
// Ret: boolean
sol::object evdev_close(sol::this_state ts, const std::string &dev_id) {
  sol::state_view lua(ts);

  auto removed = DeviceInManager::instance().detach(dev_id);
  bool ok = removed && !removed->id.empty();

  return sol::make_object(lua, ok);
}

// Lua: info(dev_id)
// Ret: table or nil
sol::object evdev_get_info(sol::this_state ts, const std::string &dev_id) {
  sol::state_view lua(ts);

  auto &state = AelkeyState::instance();
  auto it = state.input_map.find(dev_id);
  if (it == state.input_map.end()) {
    return sol::make_object(lua, sol::nil);
  }

  const InputDecl &decl = it->second;

  sol::table tbl = lua.create_table();
  tbl["id"] = decl.id;
  tbl["type"] = decl.type;
  tbl["vendor"] = decl.vendor;
  tbl["product"] = decl.product;
  tbl["bus"] = decl.bus;
  tbl["name"] = decl.name;
  tbl["phys"] = decl.phys;
  tbl["uniq"] = decl.uniq;
  tbl["grab"] = decl.grab;

  return tbl;
}

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

  mod.set_function("close", evdev_close);
  mod.set_function("info", evdev_get_info);
  mod.set_function("open", evdev_open);
  mod.set_function("send", evdev_send);
  mod.set_function("sync", evdev_sync);

  return sol::stack::push(L, mod);
}

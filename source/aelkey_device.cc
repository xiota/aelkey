#include "aelkey_device.h"

#include <sol/sol.hpp>
#include <sys/epoll.h>
#include <unistd.h>

#include "aelkey_state.h"
#include "device_declarations.h"
#include "device_input.h"
#include "device_manager.h"
#include "device_output.h"
#include "dispatcher_udev.h"

// Create all uinput output devices declared in aelkey_state.output_decls
static void create_outputs_from_decls() {
  auto &state = AelkeyState::instance();
  for (auto &out : state.output_decls) {
    if (out.id.empty()) {
      continue;
    }
    if (state.uinput_devices.count(out.id)) {
      continue;
    }

    libevdev_uinput *uidev = create_output_device(out);
    if (uidev) {
      state.uinput_devices[out.id] = uidev;
    }
  }
}

// Attach all input devices declared in aelkey_state.input_decls
static void attach_inputs_from_decls(sol::this_state ts) {
  auto &state = AelkeyState::instance();
  for (auto &decl : state.input_decls) {
    std::string devnode;
    if (!DeviceManager::instance().match(decl, devnode)) {
      continue;
    }

    if (DeviceManager::instance().attach(devnode, decl)) {
      decl.devnode = devnode;
      DispatcherUdev::instance().notify_state_change(decl, "add");
    }
  }
}

// Lua: open_device([dev_id])
// Ret: boolean
sol::object device_open(sol::this_state ts, sol::optional<std::string> dev_id_opt) {
  sol::state_view lua(ts);
  auto &state = AelkeyState::instance();

  // GLOBAL MODE: no argument → open all devices
  if (!dev_id_opt.has_value()) {
    // If devices already attached, skip
    if (!state.input_map.empty() || !state.uinput_devices.empty()) {
      return sol::make_object(lua, true);
    }

    DispatcherUdev::instance().ensure_initialized();

    // Parse declarations from Lua
    parse_outputs_from_lua(ts);
    parse_inputs_from_lua(ts);

    // Create output devices and attach input devices
    create_outputs_from_decls();
    attach_inputs_from_decls(ts);

    return sol::make_object(lua, true);
  }

  // SINGLE DEVICE MODE
  std::string dev_id = dev_id_opt.value();

  DispatcherUdev::instance().ensure_initialized();

  // Parse declarations if not already parsed
  if (state.input_decls.empty() && state.output_decls.empty()) {
    parse_outputs_from_lua(ts);
    parse_inputs_from_lua(ts);
    create_outputs_from_decls();
  }

  // Attach only the requested device
  bool ok = false;
  for (auto &decl : state.input_decls) {
    if (decl.id != dev_id) {
      continue;
    }

    std::string devnode;
    if (!DeviceManager::instance().match(decl, devnode)) {
      continue;
    }

    if (DeviceManager::instance().attach(devnode, decl)) {
      decl.devnode = devnode;
      DispatcherUdev::instance().notify_state_change(decl, "add");
      ok = true;
    }
    break;
  }

  return sol::make_object(lua, ok);
}

// Lua: close_device([dev_id])
// Ret: boolean
sol::object device_close(sol::this_state ts, const std::string &dev_id) {
  sol::state_view lua(ts);

  auto removed = DeviceManager::instance().detach(dev_id);
  bool ok = removed && !removed->id.empty();

  return sol::make_object(lua, ok);
}

// Lua: get_device_info(dev_id)
// Ret: table or nil
sol::object device_get_info(sol::this_state ts, const std::string &dev_id) {
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

#include "lua_bindings/evdev.h"

#include <string>

#include <sol/sol.hpp>

#include "aelkey_state.h"
#include "device_declarations.h"
#include "device_in_manager.h"
#include "device_out_uinput.h"

// Lua: open_device([dev_id])
// Ret: boolean
sol::object core_open_device(sol::this_state ts, sol::optional<std::string> dev_id_opt) {
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
      state.notify_state_change(decl, "add");
      ok = true;
    }
    break;
  }

  return sol::make_object(lua, ok);
}

// Lua: close_device([dev_id])
// Ret: boolean
sol::object core_close_device(sol::this_state ts, const std::string &dev_id) {
  sol::state_view lua(ts);

  auto removed = DeviceInManager::instance().detach(dev_id);
  bool ok = removed && !removed->id.empty();

  return sol::make_object(lua, ok);
}

// Lua: get_device_info(dev_id)
// Ret: table or nil
sol::object core_get_device_info(sol::this_state ts, const std::string &dev_id) {
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

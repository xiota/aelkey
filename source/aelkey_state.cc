#include "aelkey_state.h"

#include <sol/sol.hpp>

#include "device_manager.h"
#include "device_output.h"
#include "device_parser.h"
#include "dispatcher_udev.h"

void AelkeyState::attach_inputs_from_decls(sol::this_state ts) {
  for (auto &decl : input_decls) {
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

void AelkeyState::create_outputs_from_decls() {
  for (auto &out : output_decls) {
    if (out.id.empty()) {
      continue;
    }
    if (uinput_devices.count(out.id)) {
      continue;
    }

    libevdev_uinput *uidev = create_output_device(out);
    if (uidev) {
      uinput_devices[out.id] = uidev;
    }
  }
}

void AelkeyState::parse_inputs_from_lua(sol::this_state ts) {
  sol::state_view lua(ts);

  input_decls.clear();

  sol::object obj = lua["inputs"];
  if (!obj.valid() || !obj.is<sol::table>()) {
    return;
  }

  sol::table inputs = obj.as<sol::table>();

  inputs.for_each([&](sol::object /*k*/, sol::object v) {
    if (v.is<sol::table>()) {
      InputDecl decl = DeviceParser::parse_input(v.as<sol::table>());
      if (!decl.id.empty()) {
        input_decls.push_back(decl);
      }
    }
  });
}

void AelkeyState::parse_outputs_from_lua(sol::this_state ts) {
  sol::state_view lua(ts);

  output_decls.clear();

  sol::object obj = lua["outputs"];
  if (!obj.valid() || !obj.is<sol::table>()) {
    return;
  }

  sol::table outputs = obj.as<sol::table>();

  outputs.for_each([&](sol::object /*k*/, sol::object v) {
    if (v.is<sol::table>()) {
      OutputDecl decl = DeviceParser::parse_output(v.as<sol::table>());
      if (!decl.id.empty()) {
        output_decls.push_back(decl);
      }
    }
  });
}

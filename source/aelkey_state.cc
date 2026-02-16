#include "aelkey_state.h"

#include <sol/sol.hpp>
#include <sys/epoll.h>

#include "device_parser.h"
#include "lua_bindings/loop.h"
#include "manager_device_in.h"
#include "manager_device_out.h"

bool AelkeyState::on_init() {
  // initialize epoll
  if (epfd >= 0) {
    return true;
  }

  epfd = epoll_create1(EPOLL_CLOEXEC);
  if (epfd >= 0) {
    return true;
  }

  return false;
}

void AelkeyState::attach_inputs_from_decls(sol::this_state ts) {
  auto &devmgr = ManagerDeviceIn::instance();
  for (auto &decl : input_decls) {
    std::string devnode;
    if (!devmgr.match(decl, devnode)) {
      continue;
    }

    if (devmgr.attach(devnode, decl)) {
      if (decl.type != "libusb") {
        decl.devnode = devnode;
      }
    }
  }
}

void AelkeyState::create_outputs_from_decls() {
  auto &devmgr = ManagerDeviceOut::instance();
  for (auto &decl : output_decls) {
    devmgr.create(decl);
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

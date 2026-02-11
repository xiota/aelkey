#include "aelkey_state.h"

#include <sol/sol.hpp>
#include <sys/epoll.h>

#include "device_in_manager.h"
#include "device_out_manager.h"
#include "device_parser.h"

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
  auto &devmgr = DeviceInManager::instance();
  for (auto &decl : input_decls) {
    std::string devnode;
    if (!devmgr.match(decl, devnode)) {
      continue;
    }

    if (devmgr.attach(devnode, decl)) {
      decl.devnode = devnode;
      notify_state_change(decl, "add");
    }
  }
}

void AelkeyState::create_outputs_from_decls() {
  auto &devmgr = DeviceOutManager::instance();
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

void AelkeyState::notify_state_change(const InputDecl &decl, const char *state) {
  if (decl.on_state.empty()) {
    return;
  }

  sol::state_view lua(AelkeyState::instance().lua_vm);
  sol::object obj = lua[decl.on_state];
  if (!obj.is<sol::function>()) {
    return;
  }

  sol::function cb = obj.as<sol::function>();

  sol::table tbl = lua.create_table();
  tbl["device"] = decl.id;
  tbl["state"] = state ? state : "";

  sol::protected_function pf = cb;
  sol::protected_function_result result = pf(tbl);
  if (!result.valid()) {
    sol::error err = result;
    std::fprintf(stderr, "Lua state_callback error: %s\n", err.what());
  }
}

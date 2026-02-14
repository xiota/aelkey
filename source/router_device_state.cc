#include "router_device_state.h"

#include <sol/sol.hpp>

#include "aelkey_state.h"
#include "device_declarations.h"
#include "device_in_manager.h"

RouterDeviceState::RouterDeviceState() {
  tok_state_changed_ = DeviceInManager::instance().sig_state_changed_.subscribe(
      [this](const InputDecl &decl, const char *state) { notify_state_change(decl, state); }
  );
}

void RouterDeviceState::notify_state_change(const InputDecl &decl, const char *state) {
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

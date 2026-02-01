#include "aelkey_core.h"

#include <string>

#include <sol/sol.hpp>

#include "tick_scheduler.h"

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

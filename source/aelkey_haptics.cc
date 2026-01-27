#include "aelkey_haptics.h"

#include <cstdio>

#include <sol/sol.hpp>
#include <sys/ioctl.h>
#include <unistd.h>

#include "aelkey_state.h"
#include "dispatcher_haptics.h"

sol::table haptics_create(sol::table tbl) {
  auto &disp = DispatcherHaptics::instance();

  ff_effect eff = DispatcherHaptics::lua_to_ff_effect(tbl);

  int virt_id = disp.create_persistent_effect(HAPTICS_SOURCE_CUSTOM, eff);

  tbl["source"] = HAPTICS_SOURCE_CUSTOM;
  tbl["id"] = virt_id;
  return tbl;
}

void haptics_erase(sol::table tbl) {
  std::string source = tbl.get_or("source", std::string{});
  int id = tbl.get_or("id", -1);

  if (!source.empty() && id >= 0) {
    DispatcherHaptics::instance().erase_persistent_effect(source, id);
  }

  tbl["source"] = sol::nil;
  tbl["id"] = -1;
}

void haptics_play(std::string sink_id, sol::table ev) {
  auto &disp = DispatcherHaptics::instance();

  std::string source = ev.get_or("source", std::string{});
  int id = ev.get_or("id", -1);
  int magnitude = ev.get_or("value", 0);

  ff_effect eff{};
  ff_effect *maybe_eff = nullptr;

  if (source.empty() || id < 0 || !disp.get_source(source) ||
      disp.get_source(source)->effects.count(id) == 0) {
    // one-shot
    eff = DispatcherHaptics::lua_to_ff_effect(ev);
    maybe_eff = &eff;
  }

  disp.play_effect(sink_id, source, id, magnitude, maybe_eff);
}

void haptics_stop(std::string sink_id, sol::table ev) {
  std::string source = ev.get_or("source", std::string{});
  int id = ev.get_or("id", -1);

  DispatcherHaptics::instance().stop_effect(sink_id, source, id);
}

extern "C" int luaopen_aelkey_haptics(lua_State *L) {
  sol::state_view lua(L);

  sol::table mod = lua.create_table();

  mod.set_function("create", &haptics_create);
  mod.set_function("erase", &haptics_erase);
  mod.set_function("play", &haptics_play);
  mod.set_function("stop", &haptics_stop);

  return sol::stack::push(L, mod);
}

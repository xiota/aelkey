#include "aelkey_haptics.h"

#include <cstdio>

#include <sol/sol.hpp>
#include <sys/ioctl.h>
#include <unistd.h>

#include "aelkey_state.h"
#include "dispatcher_haptics.h"
#include "haptics_context.h"

void haptics_play(std::string sink_id, sol::table ev) {
  auto &state = AelkeyState::instance();

  // Find the sink device
  auto it = state.input_map.find(sink_id);
  if (it == state.input_map.end()) {
    std::fprintf(stderr, "Haptics: invalid sink_id '%s'\n", sink_id.c_str());
    return;
  }

  InputCtx &sink = it->second;
  HapticsSinkCtx &hctx = sink.haptics;

  if (!hctx.supported) {
    std::fprintf(stderr, "Haptics: sink '%s' does not support FF\n", sink_id.c_str());
    return;
  }

  // Extract source + virt_id from event
  std::string source_id = ev["source"];
  int virt_id = ev["id"];
  int magnitude = ev.get_or("value", 0);

  auto key = std::make_pair(source_id, virt_id);

  // One-shot vs Persistent effect
  HapticsSourceCtx *src_ctx = nullptr;
  ff_effect *stored_eff = nullptr;

  if (!source_id.empty() && virt_id >= 0) {
    src_ctx = DispatcherHaptics::instance().get_source(source_id);
    if (src_ctx) {
      auto eff_it = src_ctx->effects.find(virt_id);
      if (eff_it != src_ctx->effects.end()) {
        stored_eff = &eff_it->second;
      }
    }
  }

  bool persistent = (stored_eff != nullptr);

  int real_id = -1;
  if (persistent) {
    auto it_slot = hctx.slots.find(key);

    if (it_slot != hctx.slots.end()) {
      real_id = it_slot->second;
    } else {
      ff_effect eff = *stored_eff;

      real_id = DispatcherHaptics::upload_effect_to_sink(sink, hctx, eff);
      if (real_id < 0) {
        return;
      }

      hctx.slots[key] = real_id;
    }
  } else {
    ff_effect eff = DispatcherHaptics::lua_to_ff_effect(ev);

    real_id = DispatcherHaptics::upload_effect_to_sink(sink, hctx, eff);
    if (real_id < 0) {
      return;
    }

    static int internal_id_counter = 0;
    key = std::make_pair(HAPTICS_SOURCE_ONESHOT, internal_id_counter++);

    hctx.slots[key] = real_id;
  }

  // Play the effect
  struct input_event play_ev{};
  play_ev.type = EV_FF;
  play_ev.code = real_id;
  play_ev.value = magnitude;

  if (write(sink.fd, &play_ev, sizeof(play_ev)) < 0) {
    perror("write(EV_FF)");
  }
}

void haptics_stop(std::string sink_id, sol::table ev) {
  auto &state = AelkeyState::instance();

  auto it = state.input_map.find(sink_id);
  if (it == state.input_map.end()) {
    std::fprintf(stderr, "Haptics: invalid sink_id '%s'\n", sink_id.c_str());
    return;
  }

  InputCtx &sink = it->second;
  HapticsSinkCtx &hctx = sink.haptics;

  if (!hctx.supported) {
    return;
  }

  std::string source_id = ev["source"];
  int virt_id = ev["id"];

  auto key = std::make_pair(source_id, virt_id);
  auto it_slot = hctx.slots.find(key);

  if (it_slot == hctx.slots.end()) {
    return;  // nothing to stop
  }

  int real_id = it_slot->second;

  struct input_event stop_ev{};
  stop_ev.type = EV_FF;
  stop_ev.code = real_id;
  stop_ev.value = 0;

  if (write(sink.fd, &stop_ev, sizeof(stop_ev)) < 0) {
    perror("write(EV_FF)");
  }
}

sol::table haptics_create(sol::table tbl) {
  std::string source_id = HAPTICS_SOURCE_CUSTOM;  // internal bucket

  // Get/Create the internal source context
  HapticsSourceCtx *src = DispatcherHaptics::instance().get_source(source_id);
  if (!src) {
    // Create a new source bucket
    DispatcherHaptics::instance().register_source(source_id, -1, "");
    src = DispatcherHaptics::instance().get_source(source_id);
  }

  src->id = source_id;  // Ensure ID is set

  // Convert the table to our C struct
  ff_effect eff = DispatcherHaptics::lua_to_ff_effect(tbl);

  // Assign an internal ID (just increment a counter)
  static int internal_id_counter = 0;
  if (eff.id == -1) {
    eff.id = internal_id_counter++;
  }

  // If the user updates an existing effect, clear it from physical controllers
  DispatcherHaptics::instance().propagate_erase_to_sinks(source_id, eff.id);

  // Store it
  src->effects[eff.id] = eff;

  // Inject back into Lua table for the user
  tbl["source"] = source_id;
  tbl["id"] = eff.id;

  // Return the table
  return tbl;
}

void haptics_erase(sol::table tbl) {
  std::string source_id = tbl.get_or("source", std::string(""));
  int virt_id = tbl.get_or("id", -1);

  if (source_id.empty() || virt_id == -1) {
    return;  // Not a managed effect table
  }

  // Clean up the physical hardware (Sinks)
  DispatcherHaptics::instance().propagate_erase_to_sinks(source_id, virt_id);

  // Remove the definition from the Source
  HapticsSourceCtx *src = DispatcherHaptics::instance().get_source(source_id);
  if (src) {
    src->effects.erase(virt_id);
  }

  // Clean up the Lua table so it can't be accidentally played again
  tbl["source"] = sol::nil;
  tbl["id"] = -1;
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

#include "aelkey_haptics.h"

#include <cstdio>

#include <sol/sol.hpp>
#include <sys/ioctl.h>
#include <unistd.h>

#include "aelkey_state.h"

static void haptics_propagate_erase_to_sinks(const std::string &source_id, int virt_id) {
  auto &state = AelkeyState::instance();
  auto key = std::make_pair(source_id, virt_id);

  for (auto &[id, ictx] : state.input_map) {
    if (!ictx.haptics.supported) {
      continue;
    }

    auto &sink = ictx.haptics;

    auto it = sink.slots.find(key);
    if (it == sink.slots.end()) {
      continue;
    }

    int real_id = it->second;

    if (ioctl(ictx.fd, EVIOCRMFF, real_id) < 0) {
      perror("EVIOCRMFF");
    }

    sink.slots.erase(it);
  }
}

static int upload_effect_to_sink(InputCtx &sink, HapticsSinkCtx &hctx, ff_effect &eff) {
  // Always let the sink assign its own ID
  eff.id = -1;

  int rc = ioctl(sink.fd, EVIOCSFF, &eff);

  if (rc < 0 && errno == ENOSPC) {
    // Flush all existing slots
    for (const auto &[key_pair, r_id] : hctx.slots) {
      ioctl(sink.fd, EVIOCRMFF, r_id);
    }
    hctx.slots.clear();

    // Retry upload
    rc = ioctl(sink.fd, EVIOCSFF, &eff);
  }

  if (rc < 0) {
    perror("EVIOCSFF");
    return -1;
  }

  return eff.id;  // real_id assigned by sink
}

static ff_effect lua_to_ff_effect(sol::table t) {
  ff_effect eff{};
  eff.id = -1;

  eff.direction = t.get_or("direction", 0);
  eff.replay.length = t.get_or("length", 250);
  eff.replay.delay = t.get_or("delay", 0);
  eff.trigger.button = t.get_or("trigger_button", 0);
  eff.trigger.interval = t.get_or("trigger_interval", 0);

  std::string type = t.get_or("type", std::string("rumble"));
  if (type == "rumble") {
    eff.type = FF_RUMBLE;
    eff.u.rumble.strong_magnitude = t.get_or("strong", 0x4000);
    eff.u.rumble.weak_magnitude = t.get_or("weak", 0x4000);
  } else if (type == "periodic") {
    eff.type = FF_PERIODIC;
    eff.u.periodic.magnitude = t.get_or("magnitude", 0);
    eff.u.periodic.offset = t.get_or("offset", 0);
    eff.u.periodic.period = t.get_or("period", 0);
    eff.u.periodic.phase = t.get_or("phase", 0);
    eff.u.periodic.waveform = t.get_or("waveform", 0);

    eff.u.periodic.envelope.attack_length = t.get_or("attack_length", 0);
    eff.u.periodic.envelope.attack_level = t.get_or("attack_level", 0);
    eff.u.periodic.envelope.fade_length = t.get_or("fade_length", 0);
    eff.u.periodic.envelope.fade_level = t.get_or("fade_level", 0);
  } else if (type == "constant") {
    eff.type = FF_CONSTANT;
    eff.u.constant.level = t.get_or("level", 0);

    eff.u.constant.envelope.attack_length = t.get_or("attack_length", 0);
    eff.u.constant.envelope.attack_level = t.get_or("attack_level", 0);
    eff.u.constant.envelope.fade_length = t.get_or("fade_length", 0);
    eff.u.constant.envelope.fade_level = t.get_or("fade_level", 0);
  } else {
    eff.type = FF_RUMBLE;
    eff.u.rumble.strong_magnitude = 0x4000;
    eff.u.rumble.weak_magnitude = 0x4000;
    if (!eff.replay.delay) {
      eff.replay.delay = 250;
    }
  }

  return eff;
}

// Rebuild a clean, canonical, upload-ready ff_effect from a raw template.
static bool rebuild_effect(const ff_effect &src_eff, ff_effect &out_eff) {
  ff_effect eff{};
  eff.id = -1;

  eff.type = src_eff.type;
  eff.direction = src_eff.direction;
  eff.replay = src_eff.replay;
  eff.trigger = src_eff.trigger;

  switch (src_eff.type) {
    case FF_RUMBLE:
      eff.u.rumble = src_eff.u.rumble;
      break;

    case FF_PERIODIC:
      eff.u.periodic = src_eff.u.periodic;
      // Ensure envelope is copied
      eff.u.periodic.envelope = src_eff.u.periodic.envelope;
      break;

    case FF_CONSTANT:
      eff.u.constant = src_eff.u.constant;
      // Ensure envelope is copied
      eff.u.constant.envelope = src_eff.u.constant.envelope;
      break;

    default:
      // Fallback to rumble if unsupported
      eff.type = FF_RUMBLE;
      eff.u.rumble.strong_magnitude = 0x4000;
      eff.u.rumble.weak_magnitude = 0x4000;

      // Preserve replay length if present
      eff.replay.length = src_eff.replay.length ? src_eff.replay.length : 250;
      break;
  }

  out_eff = eff;
  return true;
}

// Register a virtual FF source (uinput device)
void haptics_register_source(const std::string &id, int uinput_fd) {
  auto &state = AelkeyState::instance();

  HapticsSourceCtx ctx;
  ctx.id = id;
  ctx.fd = uinput_fd;

  // Add FD to epoll
  struct epoll_event ev{};
  ev.events = EPOLLIN;
  ev.data.fd = uinput_fd;

  if (epoll_ctl(state.epfd, EPOLL_CTL_ADD, uinput_fd, &ev) < 0) {
    perror("epoll_ctl add haptics source");
  }

  state.haptics_sources[id] = std::move(ctx);
}

// Handle UI_FF_UPLOAD
bool haptics_handle_upload(HapticsSourceCtx &hctx, int request_id) {
  int fd = hctx.fd;

  struct uinput_ff_upload up{};
  up.request_id = request_id;

  if (ioctl(fd, UI_BEGIN_FF_UPLOAD, &up) < 0) {
    perror("UI_BEGIN_FF_UPLOAD");
    return false;
  }

  up.retval = 0;

  if (ioctl(fd, UI_END_FF_UPLOAD, &up) < 0) {
    perror("UI_END_FF_UPLOAD");
    return false;
  }

  int virt_id = up.effect.id;

  // Normalize the effect
  ff_effect normalized{};
  if (!rebuild_effect(up.effect, normalized)) {
    std::fprintf(stderr, "Haptics: failed to rebuild effect %d\n", virt_id);
    return false;
  }

  // Store the new effect data from the game
  hctx.effects[virt_id] = normalized;

  // Erase old real_ids on all sinks
  haptics_propagate_erase_to_sinks(hctx.id, virt_id);

  return true;
}

// Handle UI_FF_ERASE
bool haptics_handle_erase(HapticsSourceCtx &hctx, int request_id) {
  int fd = hctx.fd;

  struct uinput_ff_erase er{};
  er.request_id = request_id;

  if (ioctl(fd, UI_BEGIN_FF_ERASE, &er) < 0) {
    perror("UI_BEGIN_FF_ERASE");
    return false;
  }

  int virt_id = er.effect_id;

  // Remove effect from source
  hctx.effects.erase(virt_id);

  // Propagate erase to all sinks
  haptics_propagate_erase_to_sinks(hctx.id, virt_id);

  er.retval = 0;

  if (ioctl(fd, UI_END_FF_ERASE, &er) < 0) {
    perror("UI_END_FF_ERASE");
    return false;
  }
  return true;
}

sol::table haptics_effect_to_lua(sol::state_view lua, const ff_effect &eff) {
  sol::table t = lua.create_table();

  t["id"] = eff.id;
  t["length"] = eff.replay.length;
  t["delay"] = eff.replay.delay;

  switch (eff.type) {
    case FF_RUMBLE:
      t["type"] = "rumble";
      t["strong"] = eff.u.rumble.strong_magnitude;
      t["weak"] = eff.u.rumble.weak_magnitude;
      break;

    case FF_PERIODIC:
      t["type"] = "periodic";
      t["waveform"] = eff.u.periodic.waveform;
      t["magnitude"] = eff.u.periodic.magnitude;
      t["offset"] = eff.u.periodic.offset;
      t["phase"] = eff.u.periodic.phase;
      t["period"] = eff.u.periodic.period;
      break;

    case FF_CONSTANT:
      t["type"] = "constant";
      t["level"] = eff.u.constant.level;
      break;

    default:
      break;
  }

  return t;
}

void haptics_handle_play(
    sol::this_state ts,
    HapticsSourceCtx &src,
    int virt_id,
    int magnitude
) {
  sol::state_view lua(ts);

  if (src.callback.empty()) {
    return;
  }

  sol::object cb = lua[src.callback];
  if (!cb.is<sol::function>()) {
    return;
  }

  sol::function f = cb.as<sol::function>();

  sol::table ev = lua.create_table();
  ev["source"] = src.id;
  ev["type"] = "play";
  ev["id"] = virt_id;
  ev["value"] = magnitude;

  auto it = src.effects.find(virt_id);
  if (it != src.effects.end()) {
    ev["effect"] = haptics_effect_to_lua(lua, it->second);
  }

  sol::protected_function pf = f;
  sol::protected_function_result res = pf(ev);
  if (!res.valid()) {
    sol::error err = res;
    std::fprintf(stderr, "Lua haptics callback error: %s\n", err.what());
  }
}

void haptics_handle_stop(sol::this_state ts, HapticsSourceCtx &src, int virt_id) {
  sol::state_view lua(ts);

  if (src.callback.empty()) {
    return;
  }

  sol::object cb = lua[src.callback];
  if (!cb.is<sol::function>()) {
    return;
  }

  sol::function f = cb.as<sol::function>();

  sol::table ev = lua.create_table();
  ev["source"] = src.id;
  ev["type"] = "stop";
  ev["id"] = virt_id;

  sol::protected_function pf = f;
  sol::protected_function_result res = pf(ev);
  if (!res.valid()) {
    sol::error err = res;
    std::fprintf(stderr, "Lua haptics callback error: %s\n", err.what());
  }
}

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
    auto src_it = state.haptics_sources.find(source_id);
    if (src_it != state.haptics_sources.end()) {
      src_ctx = &src_it->second;
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

      real_id = upload_effect_to_sink(sink, hctx, eff);
      if (real_id < 0) {
        return;
      }

      hctx.slots[key] = real_id;
    }
  } else {
    ff_effect eff = lua_to_ff_effect(ev);

    real_id = upload_effect_to_sink(sink, hctx, eff);
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
  auto &state = AelkeyState::instance();
  std::string source_id = HAPTICS_SOURCE_CUSTOM;  // internal bucket

  // Get/Create the internal source context
  HapticsSourceCtx &hctx = state.haptics_sources[source_id];
  hctx.id = source_id;  // Ensure ID is set

  // Convert the table to our C struct
  ff_effect eff = lua_to_ff_effect(tbl);

  // Assign an internal ID (just increment a counter)
  static int internal_id_counter = 0;
  if (eff.id == -1) {
    eff.id = internal_id_counter++;
  }

  // If the user updates an existing effect, clear it from physical controllers
  haptics_propagate_erase_to_sinks(source_id, eff.id);

  // Store it
  hctx.effects[eff.id] = eff;

  // Inject back into Lua table for the user
  tbl["source"] = source_id;
  tbl["id"] = eff.id;

  // Return the table
  return tbl;
}

void haptics_erase(sol::table tbl) {
  auto &state = AelkeyState::instance();

  // Extract the routing info injected by 'create'
  std::string source_id = tbl.get_or("source", std::string(""));
  int virt_id = tbl.get_or("id", -1);

  if (source_id.empty() || virt_id == -1) {
    return;  // Not a managed effect table
  }

  // Clean up the physical hardware (Sinks)
  haptics_propagate_erase_to_sinks(source_id, virt_id);

  // Remove the definition from the Source
  auto it = state.haptics_sources.find(source_id);
  if (it != state.haptics_sources.end()) {
    it->second.effects.erase(virt_id);
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

#include "manager_haptics.h"

#include <cerrno>
#include <cstdio>

#include <sol/sol.hpp>

#include "haptic_sink_evdev.h"
#include "haptic_source_uinput.h"

void ManagerHaptics::register_source(
    const std::string &id,
    int uinput_fd,
    const std::string &callback
) {
  auto src = std::make_unique<HapticSourceUinput>(id, uinput_fd, callback);
  sources_[id] = std::move(src);
}

void ManagerHaptics::register_sink(const std::string &id, int evdev_fd) {
  if (evdev_fd < 0) {
    return;
  }

  auto sink = std::make_unique<HapticSinkEvdev>(id, evdev_fd);
  sinks_[id] = std::move(sink);
}

int ManagerHaptics::create_persistent_effect(const std::string &source_id, ff_effect &eff_out) {
  HapticSource *src = get_source(source_id);
  if (!src) {
    register_source(source_id, -1, "");
    src = get_source(source_id);
  }

  static int internal_id_counter = 0;
  if (eff_out.id == -1) {
    eff_out.id = internal_id_counter++;
  }

  src->sig_effect_erased_.emit(eff_out.id);
  src->get_effects()[eff_out.id] = eff_out;

  return eff_out.id;
}

bool ManagerHaptics::erase_persistent_effect(const std::string &source_id, int virt_id) {
  HapticSource *src = get_source(source_id);
  if (!src) {
    return false;
  }

  src->sig_effect_erased_.emit(virt_id);
  src->get_effects().erase(virt_id);
  return true;
}

int ManagerHaptics::play_effect(
    const std::string &sink_id,
    const std::string &source_id,
    int virt_id,
    int magnitude,
    const ff_effect *maybe_eff
) {
  HapticSink *sink = get_sink(sink_id);
  if (!sink) {
    return -1;
  }

  return sink->play_effect(source_id, virt_id, magnitude, maybe_eff, *this);
}

bool ManagerHaptics::stop_effect(
    const std::string &sink_id,
    const std::string &source_id,
    int virt_id
) {
  HapticSink *sink = get_sink(sink_id);
  if (!sink) {
    return false;
  }

  return sink->stop_effect(source_id, virt_id);
}

ff_effect ManagerHaptics::lua_to_ff_effect(sol::table t) {
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

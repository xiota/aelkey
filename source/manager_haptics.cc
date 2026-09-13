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

void ManagerHaptics::propagate_erase_to_sinks(const std::string &source_id, int virt_id) {
  auto key = std::make_pair(source_id, virt_id);

  for (auto &[sink_id, sink] : sinks_) {
    if (!sink) {
      continue;
    }
    auto &slots = sink->get_slots();
    auto it = slots.find(key);
    if (it == slots.end()) {
      continue;
    }

    int real_id = it->second;
    sink->erase_effect(real_id);
    slots.erase(it);
  }
}

void ManagerHaptics::propagate_update_to_sinks(
    const std::string &source_id,
    int virt_id,
    ff_effect &normalized
) {
  auto key = std::make_pair(source_id, virt_id);
  for (auto &[sink_id, sink] : sinks_) {
    if (!sink) {
      continue;
    }
    auto &slots = sink->get_slots();
    auto it = slots.find(key);
    if (it != slots.end()) {
      int real_id = it->second;
      sink->upload_effect(normalized, real_id);
    }
  }
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

  propagate_erase_to_sinks(source_id, eff_out.id);
  src->get_effects()[eff_out.id] = eff_out;

  return eff_out.id;
}

bool ManagerHaptics::erase_persistent_effect(const std::string &source_id, int virt_id) {
  HapticSource *src = get_source(source_id);
  if (!src) {
    return false;
  }

  propagate_erase_to_sinks(source_id, virt_id);
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
  if (!sink || sink->get_fd() < 0) {
    return -1;
  }

  int real_id = -1;
  std::string actual_source = source_id;
  int actual_virt = virt_id;

  if (maybe_eff == nullptr) {
    auto key = std::make_pair(source_id, virt_id);
    auto &slots = sink->get_slots();
    auto it_slot = slots.find(key);
    if (it_slot != slots.end()) {
      real_id = it_slot->second;
    } else {
      HapticSource *src = get_source(source_id);
      if (!src) {
        return -1;
      }

      auto &effects = src->get_effects();
      auto it = effects.find(virt_id);
      if (it == effects.end()) {
        return -1;
      }

      ff_effect eff = it->second;
      real_id = sink->upload_effect(eff);
      if (real_id < 0) {
        return -1;
      }

      slots[key] = real_id;
    }
  } else {
    static int oneshot_counter = 0;
    actual_source = HAPTICS_SOURCE_ONESHOT;
    actual_virt = oneshot_counter++;

    ff_effect eff = *maybe_eff;
    real_id = sink->upload_effect(eff);
    if (real_id < 0) {
      return -1;
    }

    auto key = std::make_pair(actual_source, actual_virt);
    sink->get_slots()[key] = real_id;
  }

  sink->play_effect_real(real_id, magnitude);

  return real_id;
}

bool ManagerHaptics::stop_effect(
    const std::string &sink_id,
    const std::string &source_id,
    int virt_id
) {
  HapticSink *sink = get_sink(sink_id);
  if (!sink || sink->get_fd() < 0) {
    return false;
  }

  auto key = std::make_pair(source_id, virt_id);
  auto &slots = sink->get_slots();
  auto it = slots.find(key);
  if (it == slots.end()) {
    return false;
  }

  int real_id = it->second;
  sink->stop_effect_real(real_id);

  return true;
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

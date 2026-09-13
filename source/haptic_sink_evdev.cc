#include "haptic_sink_evdev.h"

#include <cerrno>
#include <cstdio>

#include <sys/ioctl.h>
#include <unistd.h>

#include "manager_haptics.h"

HapticSinkEvdev::HapticSinkEvdev(std::string id, int fd) : id_(std::move(id)), fd_(fd) {
  // Automatically subscribe to any existing sources on creation
  auto &sources = ManagerHaptics::instance().get_sources_map();
  for (auto &[src_id, src] : sources) {
    if (src) {
      subscribe_to_source(*src);
    }
  }
}

HapticSinkEvdev::~HapticSinkEvdev() = default;

void HapticSinkEvdev::subscribe_to_source(HapticSource &src) {
  const std::string &src_id = src.get_id();
  if (source_tokens_.count(src_id) > 0) {
    return;
  }

  SourceTokens tokens;
  tokens.tok_update =
      src.sig_effect_updated_.subscribe([this, src_id](int virt_id, ff_effect &normalized) {
        auto key = std::make_pair(src_id, virt_id);
        auto it = slots_.find(key);
        if (it != slots_.end()) {
          int real_id = it->second;
          upload_effect(normalized, real_id);
        }
      });

  tokens.tok_erase = src.sig_effect_erased_.subscribe([this, src_id](int virt_id) {
    auto key = std::make_pair(src_id, virt_id);
    auto it = slots_.find(key);
    if (it != slots_.end()) {
      int real_id = it->second;
      erase_effect(real_id);
      slots_.erase(it);
    }
  });

  source_tokens_[src_id] = std::move(tokens);
}

int HapticSinkEvdev::upload_effect(ff_effect &eff, int real_id) {
  if (fd_ < 0) {
    return -1;
  }

  // Use the existing real ID if provided for an in-place update
  eff.id = (real_id >= 0) ? real_id : -1;
  int rc = ioctl(fd_, EVIOCSFF, &eff);

  // Fallback if ENOSPC occurs or if in-place update fails
  if (rc < 0 && (errno == ENOSPC || real_id >= 0)) {
    if (errno == ENOSPC) {
      for (const auto &[key_pair, r_id] : slots_) {
        ioctl(fd_, EVIOCRMFF, r_id);
      }
      slots_.clear();
    }

    // Force a fresh allocation if the update failed
    eff.id = -1;
    rc = ioctl(fd_, EVIOCSFF, &eff);
  }

  if (rc < 0) {
    perror("EVIOCSFF");
    return -1;
  }

  return eff.id;
}

bool HapticSinkEvdev::erase_effect(int real_id) {
  if (fd_ < 0) {
    return false;
  }
  if (ioctl(fd_, EVIOCRMFF, real_id) < 0) {
    perror("EVIOCRMFF");
    return false;
  }
  return true;
}

bool HapticSinkEvdev::play_effect_real(int real_id, int magnitude) {
  if (fd_ < 0) {
    return false;
  }

  struct input_event ev{};
  ev.type = EV_FF;
  ev.code = real_id;
  ev.value = magnitude;

  if (write(fd_, &ev, sizeof(ev)) < 0) {
    perror("write(EV_FF)");
    return false;
  }

  return true;
}

bool HapticSinkEvdev::stop_effect_real(int real_id) {
  return play_effect_real(real_id, 0);
}

int HapticSinkEvdev::play_effect(
    const std::string &source_id,
    int virt_id,
    int magnitude,
    const ff_effect *maybe_eff,
    ManagerHaptics &manager
) {
  if (fd_ < 0) {
    return -1;
  }

  int real_id = -1;

  if (maybe_eff == nullptr) {
    HapticSource *src = manager.get_source(source_id);
    if (!src) {
      return -1;
    }

    // Ensure we are subscribed to this source if registered late
    subscribe_to_source(*src);

    auto key = std::make_pair(source_id, virt_id);
    auto it_slot = slots_.find(key);
    if (it_slot != slots_.end()) {
      real_id = it_slot->second;
    } else {
      auto &effects = src->get_effects();
      auto it = effects.find(virt_id);
      if (it == effects.end()) {
        return -1;
      }

      ff_effect eff = it->second;
      real_id = upload_effect(eff);
      if (real_id < 0) {
        return -1;
      }

      slots_[key] = real_id;
    }
  } else {
    static int oneshot_counter = 0;
    std::string actual_source = HAPTICS_SOURCE_ONESHOT;
    int actual_virt = oneshot_counter++;

    ff_effect eff = *maybe_eff;
    real_id = upload_effect(eff);
    if (real_id < 0) {
      return -1;
    }

    auto key = std::make_pair(actual_source, actual_virt);
    slots_[key] = real_id;
  }

  play_effect_real(real_id, magnitude);
  return real_id;
}

bool HapticSinkEvdev::stop_effect(const std::string &source_id, int virt_id) {
  if (fd_ < 0) {
    return false;
  }

  auto key = std::make_pair(source_id, virt_id);
  auto it = slots_.find(key);
  if (it == slots_.end()) {
    return false;
  }

  int real_id = it->second;
  stop_effect_real(real_id);
  return true;
}

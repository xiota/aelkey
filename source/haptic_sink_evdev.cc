#include "haptic_sink_evdev.h"

#include <cerrno>
#include <cstdio>

#include <sys/ioctl.h>
#include <unistd.h>

HapticSinkEvdev::HapticSinkEvdev(std::string id, int fd) : id_(std::move(id)), fd_(fd) {}

HapticSinkEvdev::~HapticSinkEvdev() = default;

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

#pragma once

#include <map>
#include <string>
#include <utility>

#include <linux/input.h>

class HapticSink {
 public:
  virtual ~HapticSink() = default;

  virtual const std::string &get_id() const = 0;
  virtual int get_fd() const = 0;
  virtual std::map<std::pair<std::string, int>, int> &get_slots() = 0;
  const std::map<std::pair<std::string, int>, int> &get_slots() const {
    return const_cast<HapticSink *>(this)->get_slots();
  }

  virtual int upload_effect(ff_effect &eff, int real_id = -1) = 0;
  virtual bool erase_effect(int real_id) = 0;
  virtual bool play_effect_real(int real_id, int magnitude) = 0;
  virtual bool stop_effect_real(int real_id) = 0;

  virtual int play_effect(
      const std::string &source_id,
      int virt_id,
      int magnitude,
      const ff_effect *maybe_eff,
      class ManagerHaptics &manager
  ) = 0;
  virtual bool stop_effect(const std::string &source_id, int virt_id) = 0;
};

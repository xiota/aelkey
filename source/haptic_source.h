#pragma once

#include <map>
#include <string>

#include <linux/input.h>
#include <sol/sol.hpp>

#include "utils/signal.h"

class HapticSource {
 public:
  virtual ~HapticSource() = default;

  virtual const std::string &get_id() const = 0;
  virtual int get_fd() const = 0;
  virtual const std::string &get_callback() const = 0;
  virtual std::map<int, ff_effect> &get_effects() = 0;
  const std::map<int, ff_effect> &get_effects() const {
    return const_cast<HapticSource *>(this)->get_effects();
  }

  virtual void handle_source_event(sol::this_state ts) = 0;

  // Signals for sinks to automatically track changes
  AelkeyUtil::Signal<void(int virt_id, ff_effect &normalized)> sig_effect_updated_;
  AelkeyUtil::Signal<void(int virt_id)> sig_effect_erased_;
};

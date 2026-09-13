#pragma once

#include <map>
#include <string>

#include <linux/input.h>
#include <sol/sol.hpp>

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

  virtual void handle_source_event(sol::this_state ts, class ManagerHaptics &dispatcher) = 0;
};

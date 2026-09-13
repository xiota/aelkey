#pragma once

#include "haptic_source.h"

#include <map>
#include <string>

#include <linux/input.h>
#include <sol/sol.hpp>

class HapticSourceUinput : public HapticSource {
 public:
  HapticSourceUinput(std::string id, int fd, std::string callback);
  ~HapticSourceUinput() override;

  const std::string &get_id() const override {
    return id_;
  }
  int get_fd() const override {
    return fd_;
  }
  const std::string &get_callback() const override {
    return callback_;
  }
  std::map<int, ff_effect> &get_effects() override {
    return effects_;
  }

  void handle_source_event(sol::this_state ts, DispatcherHaptics &dispatcher) override;

 private:
  bool handle_upload(DispatcherHaptics &dispatcher, int request_id);
  bool handle_erase(DispatcherHaptics &dispatcher, int request_id);
  void handle_play(sol::this_state ts, int virt_id, int magnitude);
  void handle_stop(sol::this_state ts, int virt_id);

  static bool rebuild_effect(const ff_effect &src_eff, ff_effect &out_eff);
  static sol::table haptics_effect_to_lua(sol::state_view lua, const ff_effect &eff);

  std::string id_;
  int fd_ = -1;
  std::string callback_;
  std::map<int, ff_effect> effects_;
};

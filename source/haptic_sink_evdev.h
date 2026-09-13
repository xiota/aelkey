#pragma once

#include <map>
#include <string>
#include <utility>
#include <vector>

#include <linux/input.h>

#include "haptic_sink.h"
#include "utils/signal.h"

class HapticSinkEvdev : public HapticSink {
 public:
  HapticSinkEvdev(std::string id, int fd);
  ~HapticSinkEvdev() override;

  const std::string &get_id() const override {
    return id_;
  }
  std::map<std::pair<std::string, int>, int> &get_slots() override {
    return slots_;
  }

  int upload_effect(ff_effect &eff, int real_id = -1) override;
  bool erase_effect(int real_id) override;
  bool play_effect_real(int real_id, int magnitude) override;
  bool stop_effect_real(int real_id) override;

  int play_effect(
      const std::string &source_id,
      int virt_id,
      int magnitude,
      const ff_effect *maybe_eff,
      ManagerHaptics &manager
  ) override;
  bool stop_effect(const std::string &source_id, int virt_id) override;

 private:
  void subscribe_to_source(class HapticSource &src);

  std::string id_;
  int fd_ = -1;
  std::map<std::pair<std::string, int>, int> slots_;

  // Track signal tokens per source to maintain auto-updating state
  struct SourceTokens {
    AelkeyUtil::Signal<void(int, ff_effect &)>::Connection tok_update;
    AelkeyUtil::Signal<void(int)>::Connection tok_erase;
  };
  std::map<std::string, SourceTokens> source_tokens_;
};

#pragma once

#include <map>
#include <string>
#include <utility>

#include <linux/input.h>

#include "haptic_sink.h"

class HapticSinkEvdev : public HapticSink {
 public:
  HapticSinkEvdev(std::string id, int fd);
  ~HapticSinkEvdev() override;

  const std::string &get_id() const override {
    return id_;
  }
  int get_fd() const override {
    return fd_;
  }
  std::map<std::pair<std::string, int>, int> &get_slots() override {
    return slots_;
  }

  int upload_effect(ff_effect &eff, int real_id = -1) override;
  bool erase_effect(int real_id) override;
  bool play_effect_real(int real_id, int magnitude) override;
  bool stop_effect_real(int real_id) override;

 private:
  std::string id_;
  int fd_ = -1;
  std::map<std::pair<std::string, int>, int> slots_;
};

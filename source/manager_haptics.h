#pragma once

#include <map>
#include <memory>
#include <string>
#include <utility>

#include <linux/input.h>
#include <sol/sol.hpp>

#include "haptic_sink.h"
#include "haptic_source.h"
#include "singleton.h"

static constexpr const char *HAPTICS_SOURCE_CUSTOM = "_aelkey_haptics_custom_";
static constexpr const char *HAPTICS_SOURCE_ONESHOT = "_aelkey_haptics_oneshot_";

class ManagerHaptics : public Singleton<ManagerHaptics> {
  friend class Singleton<ManagerHaptics>;

 protected:
  ManagerHaptics() = default;
  ~ManagerHaptics() = default;

 public:
  int create_persistent_effect(const std::string &source_id, ff_effect &eff_out);

  bool erase_persistent_effect(const std::string &source_id, int virt_id);

  int play_effect(
      const std::string &sink_id,
      const std::string &source_id,
      int virt_id,
      int magnitude,
      const ff_effect *maybe_eff  // nullptr = persistent, non-null = oneshot
  );

  bool stop_effect(const std::string &sink_id, const std::string &source_id, int virt_id);

  void register_source(const std::string &id, int uinput_fd, const std::string &callback);
  void register_sink(const std::string &id, int evdev_fd);

  HapticSource *get_source(const std::string &id) {
    auto it = sources_.find(id);
    return (it != sources_.end()) ? it->second.get() : nullptr;
  }

  HapticSink *get_sink(const std::string &id) {
    auto it = sinks_.find(id);
    return (it != sinks_.end()) ? it->second.get() : nullptr;
  }

  bool is_haptics_supported(const std::string &id) {
    return sinks_.count(id) > 0;
  }

  int get_source_slot(const std::string &sink_id, const std::string &source_id, int virt_id) {
    HapticSink *sink = get_sink(sink_id);
    if (!sink) {
      return -1;
    }

    auto key = std::make_pair(source_id, virt_id);
    auto it = sink->get_slots().find(key);
    if (it == sink->get_slots().end()) {
      return -1;
    }

    return it->second;
  }

  const std::map<std::string, std::unique_ptr<HapticSource>> &get_sources_map() const {
    return sources_;
  }

  static ff_effect lua_to_ff_effect(sol::table t);

 private:
  std::map<std::string, std::unique_ptr<HapticSource>> sources_;
  std::map<std::string, std::unique_ptr<HapticSink>> sinks_;
};

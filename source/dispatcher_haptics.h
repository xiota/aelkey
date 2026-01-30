#pragma once

#include <map>
#include <string>

#include <linux/input.h>
#include <linux/uinput.h>
#include <sol/sol.hpp>

#include "aelkey_state.h"
#include "device_declarations.h"
#include "dispatcher.h"

static constexpr const char *HAPTICS_SOURCE_CUSTOM = "_aelkey_haptics_custom_";
static constexpr const char *HAPTICS_SOURCE_ONESHOT = "_aelkey_haptics_oneshot_";

struct HapticsSourceCtx {
  std::string id;  // "virt_gamepad"
  int fd = -1;     // uinput FD

  std::string callback;

  // virtual_id → ff_effect
  std::map<int, ff_effect> effects;
};

struct HapticsSinkCtx {
  std::string id;  // "gamepad"
  int fd = -1;     // evdev FD

  // key: (source_id, virt_id) → real_id
  std::map<std::pair<std::string, int>, int> slots;
};

class DispatcherHaptics : public Dispatcher<DispatcherHaptics> {
  friend class Singleton<DispatcherHaptics>;
  friend class Dispatcher<DispatcherHaptics>;

 protected:
  DispatcherHaptics() = default;
  ~DispatcherHaptics() {
    cleanup_sources();
  }

 public:
  const char *type() const override {
    return "haptics";
  }

  // High-level operations (Lua-free)
  int create_persistent_effect(
      const std::string &source_id,
      ff_effect &eff_out  // eff_out.id assigned here
  );

  bool erase_persistent_effect(const std::string &source_id, int virt_id);

  int play_effect(
      const std::string &sink_id,
      const std::string &source_id,
      int virt_id,
      int magnitude,
      const ff_effect *maybe_eff  // nullptr = persistent, non-null = oneshot
  );

  bool stop_effect(const std::string &sink_id, const std::string &source_id, int virt_id);

  // Register a virtual FF source (uinput device)
  void register_source(const std::string &id, int uinput_fd, const std::string &callback);

  void register_sink(const std::string &id, int evdev_fd);

  // Lookup by id (for Lua API layer if needed)
  HapticsSourceCtx *get_source(const std::string &id) {
    auto it = sources_.find(id);
    return (it != sources_.end()) ? &it->second : nullptr;
  }

  HapticsSinkCtx *get_sink(const std::string &id) {
    auto it = sinks_.find(id);
    return (it != sinks_.end()) ? &it->second : nullptr;
  }

  bool is_haptics_supported(const std::string &id) {
    return sinks_.count(id) > 0;
  }

  int get_source_slot(const std::string &sink_id, const std::string &source_id, int virt_id) {
    const HapticsSinkCtx *sink = get_sink(sink_id);
    if (!sink) {
      return -1;
    }

    auto key = std::make_pair(source_id, virt_id);
    auto it = sink->slots.find(key);
    if (it == sink->slots.end()) {
      return -1;
    }

    return it->second;
  }

  // Conversion helpers (shared with Lua API layer)
  static ff_effect lua_to_ff_effect(sol::table t);
  static sol::table haptics_effect_to_lua(sol::state_view lua, const ff_effect &eff);

  // EPOLL callback
  void handle_event(EpollPayload *payload, uint32_t events) override;

 private:
  // Helpers
  void propagate_erase_to_sinks(const std::string &source_id, int virt_id);
  static int upload_effect_to_sink(const std::string &sink_id, ff_effect &eff);
  static bool rebuild_effect(const ff_effect &src_eff, ff_effect &out_eff);

  void cleanup_sources();
  bool handle_upload(HapticsSourceCtx &hctx, int request_id);
  bool handle_erase(HapticsSourceCtx &hctx, int request_id);
  void handle_play(sol::this_state ts, HapticsSourceCtx &src, int virt_id, int magnitude);
  void handle_stop(sol::this_state ts, HapticsSourceCtx &src, int virt_id);

 private:
  std::map<std::string, HapticsSourceCtx> sources_;
  std::map<std::string, HapticsSinkCtx> sinks_;
};

template class Dispatcher<DispatcherHaptics>;

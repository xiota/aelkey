#pragma once

#include <string>
#include <unordered_map>

#include <linux/input.h>
#include <linux/uinput.h>
#include <sol/sol.hpp>

#include "aelkey_state.h"
#include "device_input.h"
#include "dispatcher.h"
#include "dispatcher_registry.h"
#include "haptics_context.h"

class DispatcherHaptics : public Dispatcher<DispatcherHaptics> {
  friend class Singleton<DispatcherHaptics>;

 protected:
  DispatcherHaptics() = default;
  ~DispatcherHaptics() {
    cleanup_sources();
  }

 public:
  // Register a virtual FF source (uinput device)
  void register_source(const std::string &id, int uinput_fd, const std::string &callback);

  // Lookup by id (for Lua API layer if needed)
  HapticsSourceCtx *get_source(const std::string &id) {
    auto it = sources_.find(id);
    return (it != sources_.end()) ? &it->second : nullptr;
  }

  // Conversion helpers (shared with Lua API layer)
  static ff_effect lua_to_ff_effect(sol::table t);
  static sol::table haptics_effect_to_lua(sol::state_view lua, const ff_effect &eff);

  // EPOLL callback
  void handle_event(EpollPayload *payload, uint32_t events) override;

  // Helpers
  void propagate_erase_to_sinks(const std::string &source_id, int virt_id);
  static int upload_effect_to_sink(InputCtx &sink, HapticsSinkCtx &hctx, ff_effect &eff);
  static bool rebuild_effect(const ff_effect &src_eff, ff_effect &out_eff);

 private:
  std::unordered_map<std::string, HapticsSourceCtx> sources_;

  void cleanup_sources();

  bool handle_upload(HapticsSourceCtx &hctx, int request_id);
  bool handle_erase(HapticsSourceCtx &hctx, int request_id);
  void handle_play(sol::this_state ts, HapticsSourceCtx &src, int virt_id, int magnitude);
  void handle_stop(sol::this_state ts, HapticsSourceCtx &src, int virt_id);
};

template class Dispatcher<DispatcherHaptics>;

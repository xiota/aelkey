#pragma once

#include <string>
#include <unordered_map>

#include <linux/input.h>
#include <linux/uinput.h>
#include <sol/sol.hpp>

static constexpr const char *HAPTICS_SOURCE_CUSTOM = "_aelkey_haptics_custom_";
static constexpr const char *HAPTICS_SOURCE_ONESHOT = "_aelkey_haptics_oneshot_";

struct PairHash {
  std::size_t operator()(const std::pair<std::string, int> &p) const noexcept {
    std::size_t h1 = std::hash<std::string>{}(p.first);
    std::size_t h2 = std::hash<int>{}(p.second);
    return h1 ^ (h2 << 1);
  }
};

struct PairEq {
  bool operator()(
      const std::pair<std::string, int> &a,
      const std::pair<std::string, int> &b
  ) const noexcept {
    return a.first == b.first && a.second == b.second;
  }
};

struct HapticsSourceCtx {
  std::string id;  // "virt_gamepad"
  int fd = -1;     // uinput FD

  std::string callback;

  // virtual_id → ff_effect
  std::unordered_map<int, ff_effect> effects;
};

// associated with input device via InputCtx
struct HapticsSinkCtx {
  bool supported = false;

  // key: (source_id, virt_id) → real_id
  std::unordered_map<std::pair<std::string, int>, int, PairHash, PairEq> slots;
};

void haptics_register_source(const std::string &id, int uinput_fd);
bool haptics_handle_upload(HapticsSourceCtx &hctx, int request_id);
bool haptics_handle_erase(HapticsSourceCtx &hctx, int request_id);
void haptics_debug_dump(const HapticsSourceCtx &hctx);

sol::table haptics_effect_to_lua(sol::state_view lua, const ff_effect &eff);
void haptics_handle_play(sol::this_state ts, HapticsSourceCtx &src, int virt_id, int magnitude);
void haptics_handle_stop(sol::this_state ts, HapticsSourceCtx &src, int virt_id);

extern "C" int luaopen_aelkey_haptics(lua_State *L);

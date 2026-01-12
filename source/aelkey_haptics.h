#pragma once

#include <string>
#include <unordered_map>

#include <linux/input.h>
#include <linux/uinput.h>
#include <sol/sol.hpp>

struct HapticsSourceCtx {
  std::string id;
  int fd = -1;

  std::string callback;

  // virtual_id → ff_effect
  std::unordered_map<int, ff_effect> effects;
};

void haptics_register_source(const std::string &id, int uinput_fd);
bool haptics_handle_upload(HapticsSourceCtx &hctx, int request_id);
bool haptics_handle_erase(HapticsSourceCtx &hctx, int request_id);
void haptics_debug_dump(const HapticsSourceCtx &hctx);

sol::table haptics_effect_to_lua(sol::state_view lua, const ff_effect &eff);
void haptics_handle_play(sol::this_state ts, HapticsSourceCtx &src, int virt_id, int magnitude);
void haptics_handle_stop(sol::this_state ts, HapticsSourceCtx &src, int virt_id);

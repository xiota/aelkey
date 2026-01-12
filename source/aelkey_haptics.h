#pragma once

#include <string>
#include <unordered_map>

#include <linux/input.h>
#include <linux/uinput.h>

struct HapticsSourceCtx {
  std::string id;
  int fd = -1;

  // virtual_id → ff_effect
  std::unordered_map<int, ff_effect> effects;
};

void haptics_register_source(const std::string &id, int uinput_fd);
bool haptics_handle_upload(HapticsSourceCtx &hctx, int request_id);
bool haptics_handle_erase(HapticsSourceCtx &hctx, int request_id);
void haptics_debug_dump(const HapticsSourceCtx &hctx);

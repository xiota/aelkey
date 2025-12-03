#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include <libevdev/libevdev-uinput.h>

#include "device_input.h"
#include "lua_bindings.h"

struct AelkeyState {
  int epfd = -1;
  int udev_fd = -1;
  std::unordered_map<std::string, libevdev_uinput *> uinput_devices;
  std::unordered_map<int, TickCallback> tick_callbacks;
  std::unordered_map<int, InputCtx> input_map;
  std::unordered_map<int, std::vector<struct input_event>> frames;
  std::unordered_map<std::string, int> devnode_to_fd;
  bool should_stop = false;

  std::vector<InputDecl> input_decls;
  std::vector<OutputDecl> output_decls;
};

extern AelkeyState aelkey_state;

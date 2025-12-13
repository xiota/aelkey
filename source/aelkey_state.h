#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include <libevdev/libevdev-uinput.h>
#include <libudev.h>
#include <libusb-1.0/libusb.h>

#include "aelkey_core.h"
#include "device_input.h"
#include "device_output.h"

struct AelkeyState {
  int epfd = -1;
  int udev_fd = -1;
  std::unordered_map<std::string, libevdev_uinput *> uinput_devices;
  std::unordered_map<int, TickCallback> tick_callbacks;
  std::unordered_map<int, InputCtx> input_map;
  std::unordered_map<int, std::vector<struct input_event>> frames;
  std::unordered_map<std::string, int> devnode_to_fd;

  enum ActiveMode { NONE, LOOP, DAEMON };
  ActiveMode active_mode = NONE;
  bool loop_should_stop = false;
  bool daemon_should_stop = false;

  std::vector<InputDecl> input_decls;
  std::vector<OutputDecl> output_decls;

  struct udev *g_udev = nullptr;
  struct udev_monitor *g_mon = nullptr;

  std::unordered_map<std::string, std::vector<InputDecl>> watch_map;

  libusb_context *g_libusb = nullptr;

  void aelkey_set_mode(ActiveMode mode) {
    switch (mode) {
      case LOOP:
        active_mode = LOOP;
        loop_should_stop = false;
        daemon_should_stop = true;
        break;
      case DAEMON:
        active_mode = DAEMON;
        daemon_should_stop = false;
        loop_should_stop = true;
        break;
      default:
        active_mode = NONE;
        loop_should_stop = true;
        daemon_should_stop = true;
        break;
    }
  }
};

extern AelkeyState aelkey_state;

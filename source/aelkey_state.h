#pragma once

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <dbus/dbus.h>
#include <libevdev/libevdev-uinput.h>
#include <libudev.h>
#include <libusb-1.0/libusb.h>

#include "aelkey_core.h"
#include "device_input.h"
#include "device_output.h"
#include "tick_scheduler.h"

class AelkeyState {
 public:
  static AelkeyState &instance() {
    static AelkeyState inst;
    return inst;
  }

  AelkeyState(const AelkeyState &) = delete;
  AelkeyState &operator=(const AelkeyState &) = delete;
  AelkeyState(AelkeyState &&) = delete;
  AelkeyState &operator=(AelkeyState &&) = delete;

  lua_State *lua_vm = nullptr;
  TickScheduler *scheduler = nullptr;

  int epfd = -1;
  int udev_fd = -1;
  std::unordered_map<std::string, libevdev_uinput *> uinput_devices;
  std::unordered_map<std::string, InputCtx> input_map;
  std::unordered_map<std::string, std::vector<struct input_event>> frames;

  bool loop_should_stop = false;
  int sigint = 0;

  std::vector<InputDecl> input_decls;
  std::vector<OutputDecl> output_decls;

  struct udev *g_udev = nullptr;
  struct udev_monitor *g_mon = nullptr;

  std::unordered_map<std::string, std::vector<InputDecl>> watch_map;

  libusb_context *g_libusb = nullptr;
  std::unordered_set<int> libusb_fd_set;

  DBusConnection *g_dbus_conn = nullptr;
  int g_dbus_fd = -1;

  std::string callback_watchlist;

 private:
  AelkeyState() = default;
};

#pragma once

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <dbus/dbus.h>
#include <libevdev/libevdev-uinput.h>
#include <libusb-1.0/libusb.h>

#include "aelkey_core.h"
#include "device_declarations.h"
#include "singleton.h"

class AelkeyState : public Singleton<AelkeyState> {
  friend class Singleton<AelkeyState>;

 public:
  lua_State *lua_vm = nullptr;

  int epfd = -1;
  std::unordered_map<std::string, libevdev_uinput *> uinput_devices;
  std::unordered_map<std::string, InputDecl> input_map;
  std::unordered_map<std::string, std::vector<struct input_event>> frames;

  bool loop_should_stop = false;
  int sigint = 0;

  std::vector<InputDecl> input_decls;
  std::vector<OutputDecl> output_decls;

  std::unordered_map<std::string, std::vector<InputDecl>> watch_map;

  std::string on_watchlist;

 protected:
  AelkeyState() = default;
};

#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

#include <libevdev/libevdev.h>
#include <linux/input.h>
#include <sol/sol.hpp>

#include "aelkey_state.h"
#include "backend_udev.h"
#include "device_declarations.h"
#include "device_in.h"
#include "singleton.h"
#include "utils/lua_helpers.h"
#include "utils/signal.h"

struct EvdevDeviceState {
  std::string id;
  libevdev *idev = nullptr;
  std::vector<input_event> frame;
  bool grab_needed = false;
};

struct EvdevEventPayload {
  std::string device;
  std::string type;
  std::string code;
  int value;
  uint64_t timestamp;

  sol::table to_lua(sol::state_view lua) const {
    sol::table t = lua.create_table();
    AelkeyUtil::lua_set_field(t, "device", device);
    AelkeyUtil::lua_set_field(t, "type", type);
    AelkeyUtil::lua_set_field(t, "code", code);
    AelkeyUtil::lua_set_field(t, "value", value);
    AelkeyUtil::lua_set_field(t, "timestamp", timestamp);
    return t;
  }
};

class DeviceInEvdev : public DeviceIn, public Singleton<DeviceInEvdev> {
  friend class Singleton<DeviceInEvdev>;

 protected:
  DeviceInEvdev();
  ~DeviceInEvdev();

 public:
  bool match(InputDecl &decl, std::string &devnode_out) override;
  bool attach(const std::string &devnode, InputDecl &decl) override;
  bool detach(const std::string &id) override;

 private:
  void handle_evdev_event(int fd, const InputDecl &decl);
  bool try_evdev_grab(int fd, const InputDecl &decl);

 private:
  std::map<int, EvdevDeviceState> devs_;
  std::map<std::string, int> device_ids_;

  AelkeyUtil::Signal<void(const UdevEvent &)>::Connection tok_udev_event_;
};

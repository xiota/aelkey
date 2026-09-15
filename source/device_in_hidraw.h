#pragma once

#include <map>
#include <optional>
#include <string>

#include <sol/sol.hpp>

#include "backend_udev.h"
#include "device_declarations.h"
#include "device_in.h"
#include "singleton.h"
#include "utils/lua_helpers.h"
#include "utils/signal.h"

struct HidrawEventPayload {
  std::string device;
  std::string_view data;
  int size;
  std::string status;
  uint64_t timestamp;

  sol::table to_lua(sol::state_view lua) const {
    sol::table t = lua.create_table();
    AelkeyUtil::lua_set_field(t, "device", device);
    AelkeyUtil::lua_set_field(t, "data", data);
    AelkeyUtil::lua_set_field(t, "size", size);
    AelkeyUtil::lua_set_field(t, "status", status);
    AelkeyUtil::lua_set_field(t, "timestamp", timestamp);
    return t;
  }
};

class DeviceInHidraw : public DeviceIn, public Singleton<DeviceInHidraw> {
  friend class Singleton<DeviceInHidraw>;

 protected:
  DeviceInHidraw();
  ~DeviceInHidraw() = default;

 public:
  bool match(InputDecl &decl, std::string &devnode_out) override;
  bool attach(const std::string &devnode, InputDecl &decl) override;
  bool detach(const std::string &id) override;

 private:
  int get_interface_num(const std::string &devnode);
  void handle_hidraw_event(int fd, const InputDecl &decl);

  std::map<std::string, int> devices_;  // maps id -> fd
  AelkeyUtil::Signal<void(const UdevEvent &)>::Connection tok_udev_event_;
};

#pragma once

#include <map>
#include <string>
#include <vector>

#include <readerwriterqueue.h>
#include <sol/sol.hpp>

#include "backend_libusb.h"
#include "backend_udev.h"
#include "device_declarations.h"
#include "device_in.h"
#include "singleton.h"
#include "utils/lua_helpers.h"
#include "utils/signal.h"

struct UsbEventPayload {
  std::string device;
  std::string_view data;
  int size;
  int endpoint;
  std::string transfer;
  std::string status;
  uint64_t timestamp;

  sol::table to_lua(sol::state_view lua) const {
    sol::table t = lua.create_table();
    AelkeyUtil::lua_set_field(t, "device", device);
    AelkeyUtil::lua_set_field(t, "data", data);
    AelkeyUtil::lua_set_field(t, "size", size);
    AelkeyUtil::lua_set_field(t, "endpoint", endpoint);
    AelkeyUtil::lua_set_field(t, "transfer", transfer);
    AelkeyUtil::lua_set_field(t, "status", status);
    AelkeyUtil::lua_set_field(t, "timestamp", timestamp);
    return t;
  }
};

class DeviceInLibUsb : public DeviceIn, public Singleton<DeviceInLibUsb> {
  friend class Singleton<DeviceInLibUsb>;

 protected:
  DeviceInLibUsb();
  ~DeviceInLibUsb() = default;

 public:
  bool match(InputDecl &decl, std::string &devnode_out) override;
  bool attach(const std::string &devnode, InputDecl &decl) override;
  bool detach(const std::string &id) override;

  bool on_init() override;

  void enqueue_event(const std::string &id, libusb_transfer *transfer);
  void pump_messages();

 private:
  moodycamel::ReaderWriterQueue<UsbEvent> queue_;
  int dispatch_fd_ = -1;

  // key = id
  std::map<std::string, InputDecl> input_decls_;

  AelkeyUtil::Signal<void(const UdevEvent &)>::Connection tok_udev_event_;
  AelkeyUtil::Signal<void(const UsbEvent &)>::Connection tok_usb_event_;
};

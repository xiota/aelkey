#pragma once

#include <map>
#include <optional>
#include <string>

#include "backend_udev.h"
#include "device_declarations.h"
#include "device_in.h"
#include "singleton.h"
#include "utils/signal.h"

class DeviceInHidraw : public DeviceIn, public Singleton<DeviceInHidraw> {
  friend class Singleton<DeviceInHidraw>;

 protected:
  DeviceInHidraw();
  ~DeviceInHidraw() = default;

 public:
  bool match(InputDecl &decl, std::string &devnode_out) override;
  bool attach(const std::string &devnode, InputDecl &decl) override;
  bool detach(const std::string &id) override;

  int fd() const override {
    return -1;
  }

 private:
  int get_interface_num(const std::string &devnode);
  void handle_hidraw_event(int fd, const InputDecl &decl);

  std::map<std::string, int> devices_;  // maps id -> fd
  AelkeyUtil::Signal<void(const UdevEvent &)>::Connection tok_udev_event_;
};

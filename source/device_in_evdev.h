#pragma once

#include <optional>
#include <string>

#include "aelkey_state.h"
#include "device_declarations.h"
#include "device_in.h"
#include "dispatcher_udev.h"
#include "singleton.h"
#include "utils/signal.h"

class DeviceInEvdev : public DeviceIn, public Singleton<DeviceInEvdev> {
  friend class Singleton<DeviceInEvdev>;

 protected:
  DeviceInEvdev();
  ~DeviceInEvdev() = default;

 public:
  bool match(InputDecl &decl, std::string &devnode_out) override;
  bool attach(const std::string &devnode, InputDecl &decl) override;
  bool detach(const std::string &id) override;

  int fd() const override {
    return -1;
  }

 private:
  AelkeyUtil::Signal<void(const UdevEvent &)>::Connection tok_udev_event_;
};

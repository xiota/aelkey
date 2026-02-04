#pragma once

#include <optional>
#include <string>

#include "device_declarations.h"
#include "device_helpers.h"
#include "device_in.h"
#include "singleton.h"

class DeviceInHidraw : public DeviceIn, public Singleton<DeviceInHidraw> {
  friend class Singleton<DeviceInHidraw>;

 protected:
  DeviceInHidraw() = default;
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
};

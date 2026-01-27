#pragma once

#include <optional>
#include <string>

#include "device_backend.h"
#include "device_helpers.h"
#include "device_input.h"
#include "singleton.h"

class DeviceBackendHidraw : public DeviceBackend, public Singleton<DeviceBackendHidraw> {
  friend class Singleton<DeviceBackendHidraw>;

 protected:
  DeviceBackendHidraw() = default;
  ~DeviceBackendHidraw() = default;

 public:
  bool match(const InputDecl &decl, std::string &devnode_out) override;
  bool attach(const std::string &devnode, InputDecl &decl) override;
  bool detach(const std::string &id) override;

  int fd() const override {
    return -1;
  }

 private:
  int get_interface_num(const std::string &devnode);
};

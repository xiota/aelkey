#pragma once

#include <optional>
#include <string>

#include "aelkey_state.h"
#include "device_backend.h"
#include "device_helpers.h"
#include "device_input.h"
#include "singleton.h"

class DeviceBackendEvdev : public DeviceBackend, public Singleton<DeviceBackendEvdev> {
  friend class Singleton<DeviceBackendEvdev>;

 protected:
  DeviceBackendEvdev() = default;
  ~DeviceBackendEvdev() = default;

 public:
  bool match(const InputDecl &decl, std::string &devnode_out) override;
  std::optional<InputCtx> attach(const InputDecl &decl, const std::string &devnode) override;
  bool detach(const std::string &id) override;

  int fd() const override {
    return -1;
  }
};

#pragma once

#include <optional>
#include <string>

#include "device_declarations.h"

class DeviceBackend {
 protected:
  virtual ~DeviceBackend() = default;

 public:
  virtual bool match(const InputDecl &decl, std::string &devnode_out) = 0;

  virtual bool attach(const std::string &devnode, InputDecl &decl) = 0;

  virtual bool detach(const std::string &id) = 0;

  virtual int fd() const {
    return fd_;
  }

 protected:
  int fd_ = -1;
};

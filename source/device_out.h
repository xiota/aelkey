#pragma once

#include <cstdint>
#include <vector>

#include "device_declarations.h"

class DeviceOut {
 protected:
  virtual ~DeviceOut() = default;

 public:
  virtual bool create(const OutputDecl &decl) = 0;

  virtual int fd() const {
    return fd_;
  }

 protected:
  int fd_ = -1;
};

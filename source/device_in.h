#pragma once

#include <optional>
#include <string>

#include "device_declarations.h"

class DeviceIn {
 protected:
  virtual ~DeviceIn() = default;

 public:
  virtual bool match(InputDecl &decl, std::string &devnode_out) = 0;

  virtual bool attach(const std::string &devnode, InputDecl &decl) = 0;

  virtual bool detach(const std::string &id) = 0;
};

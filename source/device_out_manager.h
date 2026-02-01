#pragma once

#include <map>
#include <string>

#include "device_declarations.h"
#include "device_out.h"
#include "singleton.h"

class DeviceOut;

class DeviceOutManager : public Singleton<DeviceOutManager> {
  friend class Singleton<DeviceOutManager>;

 public:
  DeviceOut *backend_for_type(const std::string &type);

  bool create(const OutputDecl &decl);

 protected:
  DeviceOutManager();

 private:
  std::map<std::string, DeviceOut *> backends_;
};

#pragma once

#include <map>
#include <string>

#include "device_declarations.h"
#include "device_out.h"
#include "singleton.h"

class DeviceOut;

class ManagerDeviceOut : public Singleton<ManagerDeviceOut> {
  friend class Singleton<ManagerDeviceOut>;

 public:
  DeviceOut *backend_for_type(const std::string &type);

  bool create(const OutputDecl &decl);

 protected:
  ManagerDeviceOut();

 private:
  std::map<std::string, DeviceOut *> backends_;
};

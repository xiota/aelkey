#pragma once

#include <map>
#include <optional>
#include <string>

#include "device_declarations.h"
#include "device_in.h"
#include "singleton.h"

class DeviceIn;
class DispatcherBase;

bool init_dispatcher_for_type(const std::string &type);

class DeviceInManager : public Singleton<DeviceInManager> {
  friend class Singleton<DeviceInManager>;

 public:
  DeviceIn *backend_for_type(const std::string &type);

  bool match(const InputDecl &decl, std::string &devnode_out);
  bool attach(const std::string &devnode, InputDecl &decl);
  std::optional<InputDecl> detach(const std::string &dev_id);

 protected:
  DeviceInManager();

 private:
  std::map<std::string, DeviceIn *> backends_;
};

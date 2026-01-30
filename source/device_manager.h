#pragma once

#include <map>
#include <optional>
#include <string>

#include "device_backend.h"
#include "device_declarations.h"
#include "singleton.h"

class DeviceBackend;
class DispatcherBase;

bool init_dispatcher_for_type(const std::string &type);

class DeviceManager : public Singleton<DeviceManager> {
  friend class Singleton<DeviceManager>;

 public:
  bool match(const InputDecl &decl, std::string &devnode_out);
  bool attach(const std::string &devnode, InputDecl &decl);
  std::optional<InputDecl> detach(const std::string &dev_id);

  DeviceBackend *backend_for_type(const std::string &type);

 protected:
  DeviceManager();

 private:
  std::map<std::string, DeviceBackend *> backends_;
};

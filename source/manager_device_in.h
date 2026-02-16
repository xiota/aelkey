#pragma once

#include <map>
#include <optional>
#include <string>

#include "device_declarations.h"
#include "device_in.h"
#include "singleton.h"
#include "utils/signal.h"

class DeviceIn;
class DispatcherBase;

bool init_dispatcher_for_type(const std::string &type);

class ManagerDeviceIn : public Singleton<ManagerDeviceIn> {
  friend class Singleton<ManagerDeviceIn>;

 protected:
  ManagerDeviceIn();
  ~ManagerDeviceIn() = default;

 public:
  DeviceIn *backend_for_type(const std::string &type);

  bool match(InputDecl &decl, std::string &devnode_out);
  bool attach(const std::string &devnode, InputDecl &decl);
  std::optional<InputDecl> detach(const std::string &dev_id);

 public:
  AelkeyUtil::Signal<void(const InputDecl &decl, const char *state)> sig_state_changed_;

 private:
  std::map<std::string, DeviceIn *> backends_;
};

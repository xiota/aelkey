#include "device_out_manager.h"

#include "aelkey_state.h"
#include "device_out_uinput.h"

DeviceOutManager::DeviceOutManager() {
  // Register backends
  backends_["uinput"] = &DeviceOutUinput::instance();
}

DeviceOut *DeviceOutManager::backend_for_type(const std::string &type) {
  auto it = backends_.find(type);
  return (it != backends_.end()) ? it->second : nullptr;
}

bool DeviceOutManager::create(const OutputDecl &decl) {
  DeviceOut *backend = backend_for_type(decl.type);
  return backend && backend->create(decl);
}

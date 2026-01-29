#pragma once

#include <map>
#include <optional>
#include <string>

#include "device_backend.h"
#include "device_backend_evdev.h"
#include "device_backend_gatt.h"
#include "device_backend_hidraw.h"
#include "device_backend_libusb.h"
#include "device_declarations.h"
#include "dispatcher_registry.h"
#include "singleton.h"

class DeviceManager : public Singleton<DeviceManager> {
  friend class Singleton<DeviceManager>;

 public:
  bool match(const InputDecl &decl, std::string &devnode_out) {
    DeviceBackend *backend = backend_for_type(decl.type);
    if (!backend) {
      return false;
    }
    return backend->match(decl, devnode_out);
  }

  bool attach(const std::string &devnode, InputDecl &decl) {
    auto &state = AelkeyState::instance();
    if (state.input_map.contains(decl.id)) {
      return false;
    }

    init_dispatcher_for_type(decl.type);

    DeviceBackend *backend = backend_for_type(decl.type);
    if (!backend) {
      return false;
    }

    if (!backend->attach(devnode, decl)) {
      return false;
    }

    state.input_map[decl.id] = decl;
    return true;
  }

  std::optional<InputDecl> detach(const std::string &dev_id) {
    auto &state = AelkeyState::instance();
    auto it = state.input_map.find(dev_id);
    if (it == state.input_map.end()) {
      return std::nullopt;
    }

    InputDecl &decl = it->second;

    DeviceBackend *backend = backend_for_type(decl.type);
    if (!backend) {
      return std::nullopt;
    }

    if (!backend->detach(dev_id)) {
      return std::nullopt;
    }

    std::optional<InputDecl> result{ decl };  // copy before erase

    state.input_map.erase(it);
    state.frames.erase(dev_id);

    return result;
  }

  DeviceBackend *backend_for_type(const std::string &type) {
    auto it = backends_.find(type);
    return (it != backends_.end()) ? it->second : nullptr;
  }

 protected:
  DeviceManager() {
    backends_["evdev"] = &DeviceBackendEvdev::instance();
    backends_["hidraw"] = &DeviceBackendHidraw::instance();
    backends_["libusb"] = &DeviceBackendLibUSB::instance();
    backends_["gatt"] = &DeviceBackendGATT::instance();
  }

 private:
  std::map<std::string, DeviceBackend *> backends_;
};

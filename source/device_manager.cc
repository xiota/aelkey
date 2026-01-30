#include "device_manager.h"

#include "aelkey_state.h"
#include "device_backend_evdev.h"
#include "device_backend_gatt.h"
#include "device_backend_hidraw.h"
#include "device_backend_libusb.h"
#include "device_backend_midi.h"
#include "dispatcher_evdev.h"
#include "dispatcher_gatt.h"
#include "dispatcher_haptics.h"
#include "dispatcher_hidraw.h"
#include "dispatcher_libusb.h"
#include "dispatcher_udev.h"
#include "tick_scheduler.h"

DeviceManager::DeviceManager() {
  // Register dispatchers
  DispatcherEvdev::register_self();
  DispatcherGATT::register_self();
  DispatcherHaptics::register_self();
  DispatcherHidraw::register_self();
  DispatcherLibUSB::register_self();
  DispatcherUdev::register_self();
  TickScheduler::register_self();

  // Register backends
  backends_["evdev"] = &DeviceBackendEvdev::instance();
  backends_["gatt"] = &DeviceBackendGATT::instance();
  backends_["hidraw"] = &DeviceBackendHidraw::instance();
  backends_["libusb"] = &DeviceBackendLibUSB::instance();
  backends_["midi"] = &DeviceBackendMidi::instance();
}

bool DeviceManager::match(const InputDecl &decl, std::string &devnode_out) {
  DeviceBackend *backend = backend_for_type(decl.type);
  return backend && backend->match(decl, devnode_out);
}

bool DeviceManager::attach(const std::string &devnode, InputDecl &decl) {
  auto &state = AelkeyState::instance();
  if (state.input_map.contains(decl.id)) {
    return false;
  }

  bool success = init_dispatcher_for_type(decl.type);

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

std::optional<InputDecl> DeviceManager::detach(const std::string &dev_id) {
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

  std::optional<InputDecl> result{ decl };

  state.input_map.erase(it);
  state.frames.erase(dev_id);

  return result;
}

DeviceBackend *DeviceManager::backend_for_type(const std::string &type) {
  auto it = backends_.find(type);
  return (it != backends_.end()) ? it->second : nullptr;
}

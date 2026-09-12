#include "manager_device_in.h"

#include "aelkey_state.h"
#include "device_in_audio.h"
#include "device_in_evdev.h"
#include "device_in_gatt.h"
#include "device_in_hidraw.h"
#include "device_in_libusb.h"
#include "device_in_midi.h"
#include "dispatcher_event.h"
#include "dispatcher_haptics.h"
#include "dispatcher_udev.h"
#include "tick_scheduler.h"

ManagerDeviceIn::ManagerDeviceIn() {
  // Register dispatchers
  dispatchers_["event"] = &DispatcherEvent::instance();
  dispatchers_["haptics"] = &DispatcherHaptics::instance();
  dispatchers_["tick"] = &TickScheduler::instance();
  dispatchers_["udev"] = &DispatcherUdev::instance();

  // Register backends
  backends_["audio"] = &DeviceInAudio::instance();
  backends_["evdev"] = &DeviceInEvdev::instance();
  backends_["gatt"] = &DeviceInGatt::instance();
  backends_["hidraw"] = &DeviceInHidraw::instance();
  backends_["libusb"] = &DeviceInLibUSB::instance();
  backends_["midi"] = &DeviceInMidi::instance();
}

DeviceIn *ManagerDeviceIn::backend_for_type(const std::string &type) {
  auto it = backends_.find(type);
  return (it != backends_.end()) ? it->second : nullptr;
}

bool ManagerDeviceIn::init_dispatcher_for_type(const std::string &type) {
  auto it = dispatchers_.find(type);
  DispatcherBase *dispatcher = (it != dispatchers_.end()) ? it->second : nullptr;

  if (dispatcher != nullptr) {
    return dispatcher->lazy_init();
  }
  return true;
}

void ManagerDeviceIn::dispatcher_flush_deferred() {
  for (auto &[type, dispatcher] : dispatchers_) {
    dispatcher->flush_deferred();
  }
}

bool ManagerDeviceIn::match(InputDecl &decl, std::string &devnode_out) {
  DeviceIn *backend = backend_for_type(decl.type);

  bool matched = backend && backend->match(decl, devnode_out);

  if (matched) {
    sig_state_changed_.emit(decl, "match");
  }

  return matched;
}

bool ManagerDeviceIn::attach(const std::string &devnode, InputDecl &decl) {
  auto &state = AelkeyState::instance();
  if (state.input_map.contains(decl.id)) {
    return false;
  }

  bool success = init_dispatcher_for_type(decl.type);
  if (!success) {
    return false;
  }

  DeviceIn *backend = backend_for_type(decl.type);
  if (!backend) {
    return false;
  }

  if (!backend->attach(devnode, decl)) {
    return false;
  }

  state.input_map[decl.id] = decl;
  sig_state_changed_.emit(decl, "add");
  return true;
}

std::optional<InputDecl> ManagerDeviceIn::detach(const std::string &dev_id) {
  auto &state = AelkeyState::instance();
  auto it = state.input_map.find(dev_id);
  if (it == state.input_map.end()) {
    return std::nullopt;
  }

  InputDecl &decl = it->second;

  DeviceIn *backend = backend_for_type(decl.type);
  if (!backend) {
    return std::nullopt;
  }

  if (!backend->detach(dev_id)) {
    return std::nullopt;
  }

  std::optional<InputDecl> result{ decl };

  state.input_map.erase(it);

  sig_state_changed_.emit(*result, "remove");
  return result;
}

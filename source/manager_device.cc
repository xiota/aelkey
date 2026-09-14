#include "manager_device.h"

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

#include "aelkey_state.h"

#include "device_in_audio.h"
#include "device_in_evdev.h"
#include "device_in_gatt.h"
#include "device_in_hidraw.h"
#include "device_in_libusb.h"
#include "device_in_midi.h"

#include "device_out_audio.h"
#include "device_out_midi.h"
#include "device_out_uhid.h"
#include "device_out_uinput.h"

ManagerDevice::ManagerDevice() {
  register_input<DeviceInAudio>("audio");
  register_input<DeviceInEvdev>("evdev");
  register_input<DeviceInGatt>("gatt");
  register_input<DeviceInHidraw>("hidraw");
  register_input<DeviceInLibUsb>("libusb");
  register_input<DeviceInMidi>("midi");

  register_output<DeviceOutAudio>("audio");
  register_output<DeviceOutMidi>("midi");
  register_output<DeviceOutUhid>("uhid");
  register_output<DeviceOutUinput>("uinput");
}

DeviceIn *ManagerDevice::input_backend(const std::string &type) {
  auto cached = cached_inputs_.find(type);
  if (cached != cached_inputs_.end()) {
    return cached->second;
  }

  auto it = input_getters_.find(type);
  if (it == input_getters_.end()) {
    return nullptr;
  }

  DeviceIn *ptr = it->second();
  cached_inputs_[type] = ptr;
  return ptr;
}

DeviceOut *ManagerDevice::output_backend(const std::string &type) {
  auto cached = cached_outputs_.find(type);
  if (cached != cached_outputs_.end()) {
    return cached->second;
  }

  auto it = output_getters_.find(type);
  if (it == output_getters_.end()) {
    return nullptr;
  }

  DeviceOut *ptr = it->second();
  cached_outputs_[type] = ptr;
  return ptr;
}

bool ManagerDevice::match_input(InputDecl &decl, std::string &devnode_out) {
  DeviceIn *backend = input_backend(decl.type);
  bool matched = backend && backend->match(decl, devnode_out);

  if (matched) {
    sig_state_changed_.emit(decl, "match");
  }

  return matched;
}

bool ManagerDevice::attach_input(const std::string &devnode, InputDecl &decl) {
  auto &state = AelkeyState::instance();
  if (state.input_map.contains(decl.id)) {
    return false;
  }

  DeviceIn *backend = input_backend(decl.type);
  if (!backend || !backend->attach(devnode, decl)) {
    return false;
  }

  state.input_map[decl.id] = decl;
  sig_state_changed_.emit(decl, "add");
  return true;
}

std::optional<InputDecl> ManagerDevice::detach_output(const std::string &dev_id) {
  auto &state = AelkeyState::instance();
  auto it = state.input_map.find(dev_id);
  if (it == state.input_map.end()) {
    return std::nullopt;
  }

  InputDecl &decl = it->second;
  DeviceIn *backend = input_backend(decl.type);
  if (!backend || !backend->detach(dev_id)) {
    return std::nullopt;
  }

  std::optional<InputDecl> result{ decl };
  state.input_map.erase(it);

  sig_state_changed_.emit(*result, "remove");
  return result;
}

bool ManagerDevice::create_output(const OutputDecl &decl) {
  DeviceOut *backend = output_backend(decl.type);
  return backend && backend->create(decl);
}

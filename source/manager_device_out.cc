#include "manager_device_out.h"

#include "aelkey_state.h"
#include "device_out_audio.h"
#include "device_out_midi.h"
#include "device_out_uhid.h"
#include "device_out_uinput.h"

ManagerDeviceOut::ManagerDeviceOut() {
  // Register backends
  backends_["audio"] = &DeviceOutAudio::instance();
  backends_["midi"] = &DeviceOutMidi::instance();
  backends_["uhid"] = &DeviceOutUhid::instance();
  backends_["uinput"] = &DeviceOutUinput::instance();
}

DeviceOut *ManagerDeviceOut::backend_for_type(const std::string &type) {
  auto it = backends_.find(type);
  return (it != backends_.end()) ? it->second : nullptr;
}

bool ManagerDeviceOut::create(const OutputDecl &decl) {
  DeviceOut *backend = backend_for_type(decl.type);
  return backend && backend->create(decl);
}

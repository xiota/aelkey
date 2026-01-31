#pragma once

#include <map>

#include <libevdev/libevdev-uinput.h>

#include "device_declarations.h"
#include "device_out.h"
#include "singleton.h"

class DeviceOutUinput : public DeviceOut, public Singleton<DeviceOutUinput> {
  friend class Singleton<DeviceOutUinput>;

 public:
  bool create(const OutputDecl &decl) override;

  libevdev_uinput *get(std::string id) const;

  void send(const std::string &id, int type, int code, int value);

  void sync(std::string id);

 private:
  DeviceOutUinput() = default;
  ~DeviceOutUinput() {
    for (auto &[id, dev] : devices_) {
      libevdev_uinput_destroy(dev);
    }
  }

  std::map<std::string, libevdev_uinput *> devices_;
};

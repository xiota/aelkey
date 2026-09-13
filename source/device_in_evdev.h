#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

#include <libevdev/libevdev.h>
#include <linux/input.h>

#include "aelkey_state.h"
#include "backend_udev.h"
#include "device_declarations.h"
#include "device_in.h"
#include "singleton.h"
#include "utils/signal.h"

struct EvdevDeviceState {
  std::string id;
  libevdev *idev = nullptr;
  std::vector<input_event> frame;
  bool grab_needed = false;
};

class DeviceInEvdev : public DeviceIn, public Singleton<DeviceInEvdev> {
  friend class Singleton<DeviceInEvdev>;

 protected:
  DeviceInEvdev();
  ~DeviceInEvdev();

 public:
  bool match(InputDecl &decl, std::string &devnode_out) override;
  bool attach(const std::string &devnode, InputDecl &decl) override;
  bool detach(const std::string &id) override;

 private:
  void handle_evdev_event(int fd, const InputDecl &decl);
  bool try_evdev_grab(int fd, const InputDecl &decl);

  std::map<int, EvdevDeviceState> devs_;
  std::map<std::string, int> device_ids_;

  AelkeyUtil::Signal<void(const UdevEvent &)>::Connection tok_udev_event_;
};

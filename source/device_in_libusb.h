#pragma once

#include <cstdio>
#include <format>
#include <map>
#include <stdexcept>
#include <string>

#include <libudev.h>
#include <libusb-1.0/libusb.h>

#include "aelkey_state.h"
#include "device_in.h"
#include "device_in_manager.h"
#include "dispatcher_udev.h"
#include "singleton.h"
#include "utils/signal.h"

class DeviceInLibUSB : public DeviceIn, public Singleton<DeviceInLibUSB> {
  friend class Singleton<DeviceInLibUSB>;

 protected:
  DeviceInLibUSB();
  ~DeviceInLibUSB();

 public:
  bool match(InputDecl &decl, std::string &devnode_out) override;
  bool attach(const std::string &devnode, InputDecl &decl) override;
  bool detach(const std::string &id) override;
  int fd() const override;

  bool on_init() override;

  bool matches_vidpid(const InputDecl &decl, const libusb_device_descriptor &desc) const;
  int claim_interface(libusb_device_handle *devh, int iface);

  libusb_context *context() const;
  libusb_device_handle *get_handle(const std::string &id) const;

 private:
  libusb_context *libusb_ = nullptr;
  std::map<std::string, libusb_device_handle *> devices_;

  AelkeyUtil::Signal<void(const UdevEvent &)>::Connection tok_udev_event_;
};

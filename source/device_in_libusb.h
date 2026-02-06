#pragma once

#include <cstdio>
#include <map>
#include <stdexcept>
#include <string>

#include <libusb-1.0/libusb.h>

#include "device_in.h"
#include "singleton.h"

class DeviceInLibUSB : public DeviceIn, public Singleton<DeviceInLibUSB> {
  friend class Singleton<DeviceInLibUSB>;

 protected:
  DeviceInLibUSB() = default;
  ~DeviceInLibUSB() {
    if (libusb_) {
      libusb_exit(libusb_);
      libusb_ = nullptr;
    }
  }

 public:
  bool match(InputDecl &decl, std::string &devnode_out) override {
    if (decl.type != "libusb") {
      return false;
    }

    if (decl.vid_pid.empty()) {
      return false;
    }

    // pass through ID as "devnode"
    devnode_out = decl.id;
    return true;
  }

  bool attach(const std::string &devnode, InputDecl &decl) override {
    if (!on_init()) {
      return false;
    }

    uint16_t vendor = 0;
    uint16_t product = 0;

    // Find the first matching device using vid_pid
    libusb_device **list = nullptr;
    ssize_t count = libusb_get_device_list(libusb_, &list);
    if (count < 0) {
      return false;
    }

    libusb_device_handle *handle = nullptr;

    for (ssize_t i = 0; i < count; ++i) {
      libusb_device *dev = list[i];
      libusb_device_descriptor desc;

      if (libusb_get_device_descriptor(dev, &desc) != 0) {
        continue;
      }

      uint16_t dv = desc.idVendor;
      uint16_t dp = desc.idProduct;

      bool vidpid_ok = false;
      for (auto &[v, p] : decl.vid_pid) {
        bool vendor_ok = (v == 0 || v == dv);
        bool product_ok = (p == 0 || p == dp);
        if (vendor_ok && product_ok) {
          vidpid_ok = true;
          break;
        }
      }
      if (!vidpid_ok) {
        continue;
      }

      // try to open match
      if (libusb_open(dev, &handle) == 0 && handle) {
        vendor = dv;
        product = dp;
        break;
      }
    }

    libusb_free_device_list(list, 1);
    if (!handle) {
      return false;
    }

    // If interfaces vector is empty → claim all interfaces
    if (decl.interfaces.empty()) {
      libusb_config_descriptor *cfg = nullptr;
      if (libusb_get_active_config_descriptor(libusb_get_device(handle), &cfg) == 0 && cfg) {
        for (int i = 0; i < cfg->bNumInterfaces; ++i) {
          claim_interface(handle, i);
        }
        libusb_free_config_descriptor(cfg);
      }
    } else {
      // Claim only the interfaces explicitly listed
      for (int iface : decl.interfaces) {
        claim_interface(handle, iface);
      }
    }

    // Store metadata
    decl.vendor = vendor;
    decl.product = product;

    devices_[decl.id] = handle;
    return true;
  }

  bool detach(const std::string &id) override {
    auto it = devices_.find(id);
    if (it == devices_.end()) {
      return false;
    }

    libusb_device_handle *handle = it->second;
    libusb_config_descriptor *cfg = nullptr;
    if (libusb_get_active_config_descriptor(libusb_get_device(handle), &cfg) == 0 && cfg) {
      for (int i = 0; i < cfg->bNumInterfaces; ++i) {
        libusb_release_interface(handle, i);
      }
      libusb_free_config_descriptor(cfg);
    }
    libusb_close(handle);

    devices_.erase(it);
    return true;
  }

  int fd() const override {
    // DispatcherLibUSB manages event integration
    return -1;
  }

  // Backend-specific helpers
  bool on_init() override {
    if (libusb_) {
      return true;
    }

    if (libusb_init(&libusb_) == 0) {
      return true;
    }

    return false;
  }

  int claim_interface(libusb_device_handle *devh, int iface) {
    if (iface < 0) {
      iface = 0;
    }

    // Detach kernel driver if needed
    if (libusb_kernel_driver_active(devh, iface) == 1) {
      int d = libusb_detach_kernel_driver(devh, iface);
      if (d != 0) {
        std::fprintf(
            stderr, "libusb: failed to detach kernel driver: %s\n", libusb_error_name(d)
        );
        return d;
      }
    }

    // Claim the interface
    int r = libusb_claim_interface(devh, iface);
    if (r != 0) {
      std::fprintf(
          stderr, "libusb: failed to claim interface %d: %s\n", iface, libusb_error_name(r)
      );
      return r;
    }

    return 0;
  }

  libusb_context *context() const {
    return libusb_;
  }

  libusb_device_handle *get_handle(const std::string &id) const {
    auto it = devices_.find(id);
    return (it != devices_.end()) ? it->second : nullptr;
  }

 private:
  libusb_context *libusb_ = nullptr;
  std::map<std::string, libusb_device_handle *> devices_;
};

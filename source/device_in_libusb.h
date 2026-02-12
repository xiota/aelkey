#pragma once

#include <cstdio>
#include <format>
#include <map>
#include <stdexcept>
#include <string>

#include <libudev.h>
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
    // fix in attach
    devnode_out = decl.id;
    return true;
  }

  bool attach(const std::string &devnode, InputDecl &decl) override {
    if (!on_init()) {
      return false;
    }

    libusb_device **list = nullptr;
    ssize_t count = libusb_get_device_list(libusb_, &list);
    if (count < 0) {
      return false;
    }

    uint16_t vendor = 0;
    uint16_t product = 0;

    libusb_device_handle *handle = nullptr;

    for (ssize_t i = 0; i < count; ++i) {
      libusb_device *dev = list[i];
      libusb_device_descriptor desc;

      if (libusb_get_device_descriptor(dev, &desc) != 0) {
        continue;
      }

      if (!matches_vidpid(decl, desc)) {
        continue;
      }

      // Try to open match
      if (libusb_open(dev, &handle) == 0 && handle) {
        vendor = desc.idVendor;
        product = desc.idProduct;

        uint8_t bus = libusb_get_bus_number(dev);
        uint8_t addr = libusb_get_device_address(dev);
        decl.devnode = std::format("usb:{:03}-{:03}", bus, addr);

        break;
      }
    }

    libusb_free_device_list(list, 1);
    if (!handle) {
      return false;
    }

    // Claim interfaces
    if (decl.interfaces.empty()) {
      libusb_config_descriptor *cfg = nullptr;
      if (libusb_get_active_config_descriptor(libusb_get_device(handle), &cfg) == 0 && cfg) {
        for (int i = 0; i < cfg->bNumInterfaces; ++i) {
          claim_interface(handle, i);
        }
        libusb_free_config_descriptor(cfg);
      }
    } else {
      for (int iface : decl.interfaces) {
        claim_interface(handle, iface);
      }
    }

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
    if (!handle) {
      devices_.erase(it);
      return false;
    }

    libusb_config_descriptor *cfg = nullptr;
    libusb_device *dev = libusb_get_device(handle);

    if (libusb_get_active_config_descriptor(dev, &cfg) == 0 && cfg) {
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

  bool matches_vidpid(const InputDecl &decl, const libusb_device_descriptor &desc) const {
    for (auto &[v, p] : decl.vid_pid) {
      bool vendor_ok = (v == 0 || v == desc.idVendor);
      bool product_ok = (p == 0 || p == desc.idProduct);
      if (vendor_ok && product_ok) {
        return true;
      }
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

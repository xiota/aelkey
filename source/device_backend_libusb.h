#pragma once

#include <cstdio>
#include <stdexcept>
#include <string>

#include <libusb-1.0/libusb.h>

#include "device_backend.h"
#include "singleton.h"

class DeviceBackendLibUSB : public DeviceBackend, public Singleton<DeviceBackendLibUSB> {
  friend class Singleton<DeviceBackendLibUSB>;

 protected:
  DeviceBackendLibUSB() = default;
  ~DeviceBackendLibUSB() {
    if (libusb_) {
      libusb_exit(libusb_);
      libusb_ = nullptr;
    }
  }

 public:
  bool match(const InputDecl &decl, std::string &devnode_out) override {
    // libusb devices are not discovered by path
    // Instead, match by vendor/product if provided
    if (decl.type != "libusb") {
      return false;
    }

    // For now, simply pass through the ID as the "devnode"
    devnode_out = decl.id;
    return true;
  }

  bool attach(const std::string &devnode, InputDecl &decl) override {
    ensure_initialized();

    uint16_t vendor = decl.vendor;
    uint16_t product = decl.product;

    libusb_device_handle *handle = libusb_open_device_with_vid_pid(libusb_, vendor, product);

    if (!handle) {
      return false;
    }

    if (claim_interface(handle, decl.interface) != 0) {
      libusb_close(handle);
      return false;
    }

    // Track handle internally for detach()
    devices_[decl.id] = handle;

    return true;
  }

  bool detach(const std::string &id) override {
    auto it = devices_.find(id);
    if (it == devices_.end()) {
      return false;
    }

    libusb_device_handle *handle = it->second;
    libusb_release_interface(handle, 0);
    libusb_close(handle);

    devices_.erase(it);
    return true;
  }

  int fd() const override {
    // DispatcherLibUSB manages event integration
    return -1;
  }

  // Backend-specific helpers
  void ensure_initialized() {
    if (!libusb_) {
      if (libusb_init(&libusb_) != 0) {
        throw std::runtime_error("Failed to init libusb");
      }
    }
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
  std::unordered_map<std::string, libusb_device_handle *> devices_;
};

#include "device_in_libusb.h"

#include <cstdio>
#include <format>
#include <map>
#include <stdexcept>
#include <string>

#include <libudev.h>
#include <libusb-1.0/libusb.h>

#include "aelkey_state.h"
#include "device_in.h"
#include "dispatcher_udev.h"
#include "manager_device_in.h"
#include "singleton.h"
#include "utils/signal.h"

DeviceInLibUSB::DeviceInLibUSB() {
  tok_udev_event_ =
      DispatcherUdev::instance().sig_udev_event_.subscribe([this](const UdevEvent &ev) {
        if (ev.subsystem != "usb") {
          return;
        }

        if (ev.devtype != "usb_device") {
          return;
        }

        if (ev.busnum.empty() || ev.devnum.empty()) {
          return;
        }

        std::string inst_node = "usb:" + ev.busnum + "-" + ev.devnum;

        auto &state = AelkeyState::instance();

        if (ev.action == "add") {
          libusb_device_descriptor desc{};
          desc.idVendor = static_cast<uint16_t>(strtol(ev.vid.c_str(), nullptr, 16));
          desc.idProduct = static_cast<uint16_t>(strtol(ev.pid.c_str(), nullptr, 16));

          for (auto &decl : state.input_decls) {
            if (decl.type != "libusb") {
              continue;
            }

            if (!matches_vidpid(decl, desc)) {
              continue;
            }

            if (ManagerDeviceIn::instance().attach(inst_node, decl)) {
              break;
            }
          }
        } else if (ev.action == "remove") {
          for (auto &decl : state.input_decls) {
            if (decl.type != "libusb") {
              continue;
            }

            if (decl.devnode != inst_node) {
              continue;
            }
            if (ManagerDeviceIn::instance().detach(decl.id)) {
              break;
            }
          }
        }
      });
}

DeviceInLibUSB::~DeviceInLibUSB() {
  if (libusb_) {
    libusb_exit(libusb_);
    libusb_ = nullptr;
  }
}

bool DeviceInLibUSB::match(InputDecl &decl, std::string &devnode_out) {
  if (decl.type != "libusb") {
    return false;
  }

  if (decl.vid_pid.empty()) {
    return false;
  }

  devnode_out = decl.id;
  return true;
}

bool DeviceInLibUSB::attach(const std::string &devnode, InputDecl &decl) {
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

bool DeviceInLibUSB::detach(const std::string &id) {
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

  auto &state = AelkeyState::instance();
  auto it2 = state.input_map.find(id);
  if (it2 != state.input_map.end()) {
    auto decl_copy = it2->second;
  }

  return true;
}

int DeviceInLibUSB::fd() const {
  return -1;
}

bool DeviceInLibUSB::on_init() {
  if (libusb_) {
    return true;
  }

  if (libusb_init(&libusb_) == 0) {
    return true;
  }

  return false;
}

bool DeviceInLibUSB::matches_vidpid(
    const InputDecl &decl,
    const libusb_device_descriptor &desc
) const {
  for (auto &[v, p] : decl.vid_pid) {
    bool vendor_ok = (v == 0 || v == desc.idVendor);
    bool product_ok = (p == 0 || p == desc.idProduct);
    if (vendor_ok && product_ok) {
      return true;
    }
  }
  return false;
}

int DeviceInLibUSB::claim_interface(libusb_device_handle *devh, int iface) {
  if (iface < 0) {
    iface = 0;
  }

  if (libusb_kernel_driver_active(devh, iface) == 1) {
    int d = libusb_detach_kernel_driver(devh, iface);
    if (d != 0) {
      std::fprintf(
          stderr, "libusb: failed to detach kernel driver: %s\n", libusb_error_name(d)
      );
      return d;
    }
  }

  int r = libusb_claim_interface(devh, iface);
  if (r != 0) {
    std::fprintf(
        stderr, "libusb: failed to claim interface %d: %s\n", iface, libusb_error_name(r)
    );
    return r;
  }

  return 0;
}

libusb_context *DeviceInLibUSB::context() const {
  return libusb_;
}

libusb_device_handle *DeviceInLibUSB::get_handle(const std::string &id) const {
  auto it = devices_.find(id);
  return (it != devices_.end()) ? it->second : nullptr;
}

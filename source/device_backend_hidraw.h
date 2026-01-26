#pragma once

#include <optional>
#include <string>

#include <fcntl.h>
#include <linux/hidraw.h>
#include <unistd.h>

#include "device_backend.h"
#include "device_helpers.h"
#include "device_input.h"
#include "dispatcher_hidraw.h"
#include "dispatcher_udev.h"
#include "singleton.h"

class DeviceBackendHidraw : public DeviceBackend, public Singleton<DeviceBackendHidraw> {
  friend class Singleton<DeviceBackendHidraw>;

 protected:
  DeviceBackendHidraw() = default;
  ~DeviceBackendHidraw() = default;

 public:
  bool match(const InputDecl &decl, std::string &devnode_out) {
    if (decl.type != "hidraw") {
      return false;
    }

    std::string result = DispatcherUdev::instance().enumerate_and_match(
        "hidraw", [&](struct udev_device *dev) -> std::string {
          const char *devnode = udev_device_get_devnode(dev);
          if (!devnode) {
            return {};
          }

          int fd = open(devnode, O_RDONLY | O_NONBLOCK);
          if (fd < 0) {
            return {};
          }

          struct hidraw_devinfo info;
          bool ok = false;

          if (ioctl(fd, HIDIOCGRAWINFO, &info) == 0) {
            ok = true;

            if (decl.bus && static_cast<int>(info.bustype) != decl.bus) {
              ok = false;
            }

            int vendor = static_cast<unsigned short>(info.vendor);
            int product = static_cast<unsigned short>(info.product);

            if (decl.vendor && vendor != decl.vendor) {
              ok = false;
            }
            if (decl.product && product != decl.product) {
              ok = false;
            }

            if (!decl.name.empty()) {
              char name[256] = { 0 };
              if (ioctl(fd, HIDIOCGRAWNAME(sizeof(name) - 1), name) >= 0) {
                if (!match_string(decl.name, name)) {
                  ok = false;
                }
              } else {
                ok = false;
              }
            }

            if (!decl.phys.empty()) {
              char phys[64] = { 0 };
              if (ioctl(fd, HIDIOCGRAWPHYS(sizeof(phys) - 1), phys) >= 0) {
                if (!match_string(decl.phys, phys)) {
                  ok = false;
                }
              }
            }

            if (!decl.uniq.empty()) {
              char uniq[64] = { 0 };
              if (ioctl(fd, HIDIOCGRAWUNIQ(sizeof(uniq) - 1), uniq) >= 0) {
                if (!match_string(decl.uniq, uniq)) {
                  ok = false;
                }
              }
            }

            if (decl.interface >= 0) {
              int iface = get_interface_num(devnode);
              if (iface != decl.interface) {
                ok = false;
              }
            }
          }

          close(fd);

          if (ok) {
            return std::string(devnode);
          }

          return {};
        }
    );

    if (!result.empty()) {
      devnode_out = result;
      return true;
    }

    return false;
  }

  std::optional<InputCtx> attach(const InputDecl &decl, const std::string &devnode) override {
    int fd = DispatcherHidraw::instance().open_device(devnode, decl);
    if (fd < 0) {
      return std::nullopt;
    }

    InputCtx ctx;
    ctx.decl = decl;
    ctx.decl.devnode = devnode;
    ctx.active = true;
    return ctx;
  }

  bool detach(const std::string &id) override {
    DispatcherHidraw::instance().remove_device(id);
    return true;
  }

  int fd() const override {
    // DispatcherHidraw manages event integration
    return -1;
  }

 private:
  int get_interface_num(const std::string &devnode) {
    // Use the centralized udev context
    auto &udevdisp = DispatcherUdev::instance();
    udevdisp.ensure_initialized();  // make sure context exists

    struct udev *udev = udevdisp.get_udev();  // <-- centralized context

    // Create udev device object from the hidraw node
    struct udev_device *dev = udev_device_new_from_subsystem_sysname(
        udev, "hidraw", devnode.substr(devnode.find_last_of('/') + 1).c_str()
    );
    if (!dev) {
      return -1;
    }

    const char *iface_str = udev_device_get_property_value(dev, "ID_USB_INTERFACE_NUM");
    int iface = -1;

    if (iface_str) {
      iface = std::stoi(iface_str, nullptr, 16);  // hex string like "01"
    }

    udev_device_unref(dev);
    return iface;
  }
};

#include "device_in_hidraw.h"

#include <fcntl.h>
#include <libudev.h>
#include <linux/hidraw.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "aelkey_state.h"
#include "dispatcher_hidraw.h"
#include "dispatcher_udev.h"
#include "manager_device_in.h"
#include "utils/regex_match.h"
#include "utils/signal.h"

DeviceInHidraw::DeviceInHidraw() {
  tok_udev_event_ =
      DispatcherUdev::instance().sig_udev_event_.subscribe([this](const UdevEvent &ev) {
        if (ev.subsystem != "hidraw") {
          return;
        }

        auto &state = AelkeyState::instance();

        if (ev.action == "add") {
          for (auto &decl : state.input_decls) {
            if (decl.type != "hidraw") {
              continue;
            }

            std::string matched;
            if (!match(decl, matched)) {
              continue;
            }

            if (matched != ev.devnode) {
              continue;
            }

            if (ManagerDeviceIn::instance().attach(matched, decl)) {
              break;
            }
          }
        }

        else if (ev.action == "remove") {
          for (auto &decl : state.input_decls) {
            if (decl.type != "hidraw") {
              continue;
            }

            if (decl.devnode != ev.devnode) {
              continue;
            }

            if (ManagerDeviceIn::instance().detach(decl.id)) {
              break;
            }
          }
        }
      });
}

bool DeviceInHidraw::match(InputDecl &decl, std::string &devnode_out) {
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

          int dev_vendor = static_cast<unsigned short>(info.vendor);
          int dev_product = static_cast<unsigned short>(info.product);

          // vid_pid matching
          bool vidpid_ok = decl.vid_pid.empty();
          for (auto &[v, p] : decl.vid_pid) {
            bool vendor_ok = (v == 0 || v == dev_vendor);
            bool product_ok = (p == 0 || p == dev_product);
            if (vendor_ok && product_ok) {
              vidpid_ok = true;
              break;
            }
          }
          if (!vidpid_ok) {
            ok = false;
          }

          if (ok && decl.bus && static_cast<int>(info.bustype) != decl.bus) {
            ok = false;
          }

          if (ok && !decl.name.empty()) {
            char name[256] = { 0 };
            if (ioctl(fd, HIDIOCGRAWNAME(sizeof(name) - 1), name) >= 0) {
              if (!AelkeyUtil::match_string(decl.name, name)) {
                ok = false;
              }
            } else {
              ok = false;
            }
          }

          if (ok && !decl.phys.empty()) {
            char phys[64] = { 0 };
            if (ioctl(fd, HIDIOCGRAWPHYS(sizeof(phys) - 1), phys) >= 0) {
              if (!AelkeyUtil::match_string(decl.phys, phys)) {
                ok = false;
              }
            }
          }

          if (ok && !decl.uniq.empty()) {
            char uniq[64] = { 0 };
            if (ioctl(fd, HIDIOCGRAWUNIQ(sizeof(uniq) - 1), uniq) >= 0) {
              if (!AelkeyUtil::match_string(decl.uniq, uniq)) {
                ok = false;
              }
            }
          }

          if (ok && !decl.interfaces.empty()) {
            int iface = get_interface_num(devnode);

            bool match = false;
            for (int want : decl.interfaces) {
              if (want == iface) {
                match = true;
                break;
              }
            }

            if (!match) {
              ok = false;
            }
          }

          // store vendor/product after match
          if (ok) {
            decl.vendor = dev_vendor;
            decl.product = dev_product;
          }
        }

        close(fd);

        return ok ? std::string(devnode) : std::string{};
      }
  );

  if (!result.empty()) {
    devnode_out = result;
    return true;
  }

  return false;
}

bool DeviceInHidraw::attach(const std::string &devnode, InputDecl &decl) {
  int fd = DispatcherHidraw::instance().open_device(devnode, decl);
  if (fd < 0) {
    return false;
  }

  decl.devnode = devnode;
  decl.fd = fd;
  return true;
}

bool DeviceInHidraw::detach(const std::string &id) {
  DispatcherHidraw::instance().remove_device(id);

  auto &state = AelkeyState::instance();
  auto it = state.input_map.find(id);
  if (it != state.input_map.end()) {
    auto decl_copy = it->second;
  }

  return true;
}

int DeviceInHidraw::get_interface_num(const std::string &devnode) {
  struct udev *udev = DispatcherUdev::instance().get_udev();

  struct udev_device *dev = udev_device_new_from_subsystem_sysname(
      udev, "hidraw", devnode.substr(devnode.find_last_of('/') + 1).c_str()
  );
  if (!dev) {
    return -1;
  }

  const char *iface_str = udev_device_get_property_value(dev, "ID_USB_INTERFACE_NUM");
  int iface = -1;

  if (iface_str) {
    iface = std::stoi(iface_str, nullptr, 16);
  }

  udev_device_unref(dev);

  return iface;
}

#include "device_in_evdev.h"

#include <fcntl.h>
#include <libevdev/libevdev.h>
#include <unistd.h>

#include "dispatcher_evdev.h"
#include "dispatcher_udev.h"

bool DeviceInEvdev::match(InputDecl &decl, std::string &devnode_out) {
  if (decl.type != "evdev") {
    return false;
  }

  std::string result = DispatcherUdev::instance().enumerate_and_match(
      "input", [&](struct udev_device *dev) -> std::string {
        const char *devnode = udev_device_get_devnode(dev);
        if (!devnode) {
          return {};
        }

        int fd = ::open(devnode, O_RDONLY | O_NONBLOCK);
        if (fd < 0) {
          return {};
        }

        struct libevdev *evdev = nullptr;
        bool ok = false;

        if (libevdev_new_from_fd(fd, &evdev) == 0) {
          ok = true;

          int dev_vendor = libevdev_get_id_vendor(evdev);
          int dev_product = libevdev_get_id_product(evdev);

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

          if (decl.bus && libevdev_get_id_bustype(evdev) != decl.bus) {
            ok = false;
          }

          if (!decl.name.empty() && !match_string(decl.name, libevdev_get_name(evdev) ?: "")) {
            ok = false;
          }

          if (!decl.phys.empty() && !match_string(decl.phys, libevdev_get_phys(evdev) ?: "")) {
            ok = false;
          }

          if (!decl.uniq.empty() && !match_string(decl.uniq, libevdev_get_uniq(evdev) ?: "")) {
            ok = false;
          }

          for (auto &[type, code] : decl.capabilities) {
            if (!libevdev_has_event_code(evdev, type, code)) {
              ok = false;
              break;
            }
          }

          // store vendor/product after match
          if (ok) {
            decl.vendor = dev_vendor;
            decl.product = dev_product;
          }
        }

        libevdev_free(evdev);
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

bool DeviceInEvdev::attach(const std::string &devnode, InputDecl &decl) {
  if (DispatcherEvdev::instance().open_device(devnode, decl)) {
    return true;
  }

  return false;
}

bool DeviceInEvdev::detach(const std::string &id) {
  auto &state = AelkeyState::instance();
  auto it = state.input_map.find(id);
  if (it == state.input_map.end()) {
    return false;
  }

  DispatcherEvdev::instance().close_device(it->second);
  return true;
}

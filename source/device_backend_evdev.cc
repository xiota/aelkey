#include "device_backend_evdev.h"

#include <fcntl.h>
#include <libevdev/libevdev.h>
#include <unistd.h>

#include "dispatcher_evdev.h"
#include "dispatcher_udev.h"

bool DeviceBackendEvdev::match(const InputDecl &decl, std::string &devnode_out) {
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

          if (decl.bus && libevdev_get_id_bustype(evdev) != decl.bus) {
            ok = false;
          }
          if (decl.vendor && libevdev_get_id_vendor(evdev) != decl.vendor) {
            ok = false;
          }
          if (decl.product && libevdev_get_id_product(evdev) != decl.product) {
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

std::optional<InputCtx>
DeviceBackendEvdev::attach(const InputDecl &decl, const std::string &devnode) {
  InputCtx ctx;
  ctx.decl = decl;

  if (DispatcherEvdev::instance().open_device(devnode, ctx)) {
    ctx.active = true;
    return ctx;
  }

  return std::nullopt;
}

bool DeviceBackendEvdev::detach(const std::string &id) {
  auto &state = AelkeyState::instance();
  auto it = state.input_map.find(id);
  if (it == state.input_map.end()) {
    return false;
  }

  DispatcherEvdev::instance().close_device(it->second);
  return true;
}

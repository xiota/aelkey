#include "dispatcher_udev.h"

#include <cstring>
#include <iostream>
#include <stdexcept>

#include <sol/sol.hpp>
#include <sys/epoll.h>

#include "aelkey_state.h"
#include "device_declarations.h"
#include "device_in_libusb.h"
#include "device_in_manager.h"
#include "dispatcher_registry.h"

DispatcherUdev::~DispatcherUdev() {
  if (mon_) {
    udev_monitor_unref(mon_);
    mon_ = nullptr;
  }
  if (udev_ctx_) {
    udev_unref(udev_ctx_);
    udev_ctx_ = nullptr;
  }
  mon_fd_ = -1;
}

const char *DispatcherUdev::type() const {
  return "udev";
}

bool DispatcherUdev::on_init() {
  if (udev_ctx_) {
    return true;
  }

  udev_ctx_ = udev_new();
  if (!udev_ctx_) {
    return false;
  }

  mon_ = udev_monitor_new_from_netlink(udev_ctx_, "udev");
  if (!mon_) {
    udev_unref(udev_ctx_);
    udev_ctx_ = nullptr;
    return false;
  }

  udev_monitor_filter_add_match_subsystem_devtype(mon_, "input", nullptr);
  udev_monitor_filter_add_match_subsystem_devtype(mon_, "hidraw", nullptr);
  udev_monitor_filter_add_match_subsystem_devtype(mon_, "usb", nullptr);
  udev_monitor_enable_receiving(mon_);

  mon_fd_ = udev_monitor_get_fd(mon_);
  if (mon_fd_ < 0) {
    udev_monitor_unref(mon_);
    udev_ctx_ = nullptr;
    return false;
  }

  register_fd(mon_fd_, EPOLLIN);
  return true;
}

void DispatcherUdev::handle_event(EpollPayload *, uint32_t events) {
  if (!(events & EPOLLIN) || !mon_) {
    return;
  }

  struct udev_device *dev = udev_monitor_receive_device(mon_);
  if (!dev) {
    return;
  }

  const char *action = udev_device_get_action(dev);
  if (action) {
    if (strcmp(action, "add") == 0) {
      handle_udev_add(dev);
    } else if (strcmp(action, "remove") == 0) {
      handle_udev_remove(dev);
    }
  }

  udev_device_unref(dev);
}

std::string DispatcherUdev::enumerate_and_match(
    const char *subsystem,
    const std::function<std::string(struct udev_device *)> &matcher
) {
  struct udev_enumerate *enumerate = udev_enumerate_new(udev_ctx_);
  if (!enumerate) {
    return {};
  }

  udev_enumerate_add_match_subsystem(enumerate, subsystem);
  udev_enumerate_scan_devices(enumerate);

  struct udev_list_entry *devices = udev_enumerate_get_list_entry(enumerate);
  struct udev_list_entry *entry;

  udev_list_entry_foreach(entry, devices) {
    const char *path = udev_list_entry_get_name(entry);
    struct udev_device *dev = udev_device_new_from_syspath(udev_ctx_, path);
    if (!dev) {
      continue;
    }

    std::string result = matcher(dev);
    udev_device_unref(dev);

    if (!result.empty()) {
      udev_enumerate_unref(enumerate);
      return result;
    }
  }

  udev_enumerate_unref(enumerate);
  return {};
}

struct udev *DispatcherUdev::get_udev() const {
  return udev_ctx_;
}

void DispatcherUdev::handle_udev_add(struct udev_device *dev) {
  const char *subsystem = udev_device_get_subsystem(dev);
  const char *node = udev_device_get_devnode(dev);
  std::string devnode = node ? node : "";

  if (!subsystem) {
    return;
  }

  auto &state = AelkeyState::instance();

  // Watchlist
  for (auto &entry : state.watch_map) {
    for (auto &decl : entry.second) {
      std::string matched;
      if (!DeviceInManager::instance().match(decl, matched)) {
        continue;
      }

      if ((decl.type == "evdev" && std::string(subsystem) == "input") ||
          (decl.type == "hidraw" && std::string(subsystem) == "hidraw")) {
        if (matched == devnode) {
          decl.devnode = devnode;
          decl.on_state = state.on_watchlist;
          state.notify_state_change(decl, "add");
        }
      } else if (decl.type == "libusb" && std::string(subsystem) == "usb") {
        const char *syspath = udev_device_get_syspath(dev);
        if (!syspath) {
          continue;
        }

        if (matched == std::string(syspath)) {
          decl.devnode = syspath;
          decl.on_state = state.on_watchlist;
          state.notify_state_change(decl, "add");
        }
      }
    }
  }

  // Normal devices
  for (auto &decl : state.input_decls) {
    std::string matched;
    if (!DeviceInManager::instance().match(decl, matched)) {
      continue;
    }

    if ((decl.type == "evdev" && std::string(subsystem) == "input") ||
        (decl.type == "hidraw" && std::string(subsystem) == "hidraw")) {
      if (matched == devnode) {
        if (state.input_map.contains(decl.id)) {
          break;
        }

        if (DeviceInManager::instance().attach(devnode, decl)) {
          decl.devnode = devnode;
          state.notify_state_change(decl, "add");
        }
        break;
      }
    } else if (decl.type == "libusb" && std::string(subsystem) == "usb") {
      const char *devtype = udev_device_get_devtype(dev);
      if (!devtype || strcmp(devtype, "usb_device") != 0) {
        continue;  // ignore interface-level add events
      }

      // Get VID/PID
      const char *vid = udev_device_get_property_value(dev, "ID_VENDOR_ID");
      const char *pid = udev_device_get_property_value(dev, "ID_MODEL_ID");
      if (!vid || !pid) {
        continue;
      }

      uint16_t vendor = strtol(vid, nullptr, 16);
      uint16_t product = strtol(pid, nullptr, 16);

      // build a descriptor
      libusb_device_descriptor desc{};
      desc.idVendor = vendor;
      desc.idProduct = product;

      if (!DeviceInLibUSB::instance().matches_vidpid(decl, desc)) {
        continue;
      }

      if (state.input_map.contains(decl.id)) {
        break;
      }

      if (DeviceInManager::instance().attach(matched, decl)) {
        decl.devnode = matched;
        state.notify_state_change(decl, "add");
      }

      // look for more matches
      continue;
    }
  }
}

void DispatcherUdev::handle_udev_remove(struct udev_device *dev) {
  const char *subsystem = udev_device_get_subsystem(dev);
  const char *node = udev_device_get_devnode(dev);
  std::string devnode = node ? node : "";

  if (!subsystem) {
    return;
  }

  auto &state = AelkeyState::instance();

  // Watchlist
  for (auto &entry : state.watch_map) {
    for (auto &decl : entry.second) {
      if (decl.type == "libusb" && std::string(subsystem) == "usb") {
        const char *syspath = udev_device_get_syspath(dev);
        if (!syspath) {
          continue;
        }

        if (decl.devnode == std::string(syspath)) {
          decl.on_state = state.on_watchlist;
          state.notify_state_change(decl, "remove");
          decl.devnode.clear();
        }
      } else if ((decl.type == "evdev" && std::string(subsystem) == "input") ||
                 (decl.type == "hidraw" && std::string(subsystem) == "hidraw")) {
        if (decl.devnode == devnode) {
          decl.on_state = state.on_watchlist;
          state.notify_state_change(decl, "remove");
          decl.devnode.clear();
        }
      }
    }
  }

  // Normal devices
  for (auto &decl : state.input_decls) {
    if (decl.type == "libusb" && std::string(subsystem) == "usb") {
      const char *devtype = udev_device_get_devtype(dev);
      if (!devtype || strcmp(devtype, "usb_device") != 0) {
        continue;
      }

      const char *bus_str = udev_device_get_property_value(dev, "BUSNUM");
      const char *dev_str = udev_device_get_property_value(dev, "DEVNUM");
      if (!bus_str || !dev_str) {
        continue;
      }

      std::string inst_node = std::string("usb:") + bus_str + "-" + dev_str;

      if (decl.devnode == inst_node) {
        auto removed = DeviceInManager::instance().detach(decl.id);
        if (removed && !removed->id.empty()) {
          state.notify_state_change(*removed, "remove");
        }
        break;
      }
    } else if ((decl.type == "evdev" && std::string(subsystem) == "input") ||
               (decl.type == "hidraw" && std::string(subsystem) == "hidraw")) {
      if (decl.devnode == devnode) {
        auto removed = DeviceInManager::instance().detach(decl.id);
        if (removed && !removed->id.empty()) {
          state.notify_state_change(*removed, "remove");
        }
        break;
      }
    }
  }
}

#pragma once

#include <iostream>
#include <string>

#include <libudev.h>
#include <sol/sol.hpp>

#include "aelkey_state.h"
#include "device_input.h"
#include "dispatcher.h"
#include "dispatcher_registry.h"

class DispatcherUdev : public Dispatcher<DispatcherUdev> {
  friend class Singleton<DispatcherUdev>;

 protected:
  DispatcherUdev() = default;
  ~DispatcherUdev() {
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

 public:
  // Initialization
  void ensure_initialized() {
    if (udev_ctx_) {
      return;
    }

    udev_ctx_ = udev_new();
    if (!udev_ctx_) {
      throw std::runtime_error("udev_new failed");
    }

    mon_ = udev_monitor_new_from_netlink(udev_ctx_, "udev");
    if (!mon_) {
      udev_unref(udev_ctx_);
      udev_ctx_ = nullptr;
      throw std::runtime_error("udev_monitor_new_from_netlink failed");
    }

    udev_monitor_filter_add_match_subsystem_devtype(mon_, "input", nullptr);
    udev_monitor_filter_add_match_subsystem_devtype(mon_, "hidraw", nullptr);
    udev_monitor_filter_add_match_subsystem_devtype(mon_, "usb", nullptr);
    udev_monitor_enable_receiving(mon_);

    mon_fd_ = udev_monitor_get_fd(mon_);
    if (mon_fd_ < 0) {
      udev_monitor_unref(mon_);
      udev_ctx_ = nullptr;
      throw std::runtime_error("udev_monitor_get_fd failed");
    }

    register_fd(mon_fd_, EPOLLIN);
  }

  // EPOLL callback
  void handle_event(EpollPayload *, uint32_t events) override {
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

  // Centralized enumeration
  std::string enumerate_and_match(
      const char *subsystem,
      const std::function<std::string(struct udev_device *)> &matcher
  ) {
    ensure_initialized();

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

  struct udev *get_udev() const {
    return udev_ctx_;
  }

  // Lua helper
  void notify_state_change(const InputDecl &decl, const char *state) {
    if (decl.on_state.empty()) {
      return;
    }

    sol::state_view lua(AelkeyState::instance().lua_vm);
    sol::object obj = lua[decl.on_state];
    if (!obj.is<sol::function>()) {
      return;
    }

    sol::function cb = obj.as<sol::function>();

    sol::table tbl = lua.create_table();
    tbl["device"] = decl.id;
    tbl["state"] = state ? state : "";

    sol::protected_function pf = cb;
    sol::protected_function_result result = pf(tbl);
    if (!result.valid()) {
      sol::error err = result;
      std::fprintf(stderr, "Lua state_callback error: %s\n", err.what());
    }
  }

 private:
  // UDEV ADD
  void handle_udev_add(struct udev_device *dev) {
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
        std::string matched = match_device(decl);

        if ((decl.type == "evdev" && std::string(subsystem) == "input") ||
            (decl.type == "hidraw" && std::string(subsystem) == "hidraw")) {
          if (matched == devnode) {
            decl.devnode = devnode;
            decl.on_state = state.on_watchlist;
            notify_state_change(decl, "add");
          }
        } else if (decl.type == "libusb" && std::string(subsystem) == "usb") {
          const char *syspath = udev_device_get_syspath(dev);
          if (!syspath) {
            continue;
          }

          if (matched == std::string(syspath)) {
            decl.devnode = syspath;
            decl.on_state = state.on_watchlist;
            notify_state_change(decl, "add");
          }
        }
      }
    }

    // Normal devices
    for (auto &decl : state.input_decls) {
      std::string matched = match_device(decl);

      if ((decl.type == "evdev" && std::string(subsystem) == "input") ||
          (decl.type == "hidraw" && std::string(subsystem) == "hidraw")) {
        if (matched == devnode) {
          if (state.input_map.contains(decl.id)) {
            break;
          }

          if (attach_input_device(devnode, decl)) {
            decl.devnode = devnode;
            notify_state_change(decl, "add");
          }
          break;
        }
      } else if (decl.type == "libusb" && std::string(subsystem) == "usb") {
        const char *syspath = udev_device_get_syspath(dev);
        if (!syspath) {
          continue;
        }

        if (matched == std::string(syspath)) {
          if (state.input_map.contains(decl.id)) {
            break;
          }

          if (attach_input_device(matched, decl)) {
            decl.devnode = matched;
            notify_state_change(decl, "add");
          }
          break;
        }
      }
    }
  }

  // UDEV REMOVE
  void handle_udev_remove(struct udev_device *dev) {
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
            notify_state_change(decl, "remove");
            decl.devnode.clear();
          }
        } else if ((decl.type == "evdev" && std::string(subsystem) == "input") ||
                   (decl.type == "hidraw" && std::string(subsystem) == "hidraw")) {
          if (decl.devnode == devnode) {
            decl.on_state = state.on_watchlist;
            notify_state_change(decl, "remove");
            decl.devnode.clear();
          }
        }
      }
    }

    // Normal devices
    for (auto &decl : state.input_decls) {
      if (decl.type == "libusb" && std::string(subsystem) == "usb") {
        const char *syspath = udev_device_get_syspath(dev);
        if (!syspath) {
          continue;
        }

        if (decl.devnode == std::string(syspath)) {
          InputDecl removed = detach_input_device(decl.id);
          if (!removed.id.empty()) {
            notify_state_change(removed, "remove");
          }
          break;
        }
      } else if ((decl.type == "evdev" && std::string(subsystem) == "input") ||
                 (decl.type == "hidraw" && std::string(subsystem) == "hidraw")) {
        if (decl.devnode == devnode) {
          InputDecl removed = detach_input_device(decl.id);
          if (!removed.id.empty()) {
            notify_state_change(removed, "remove");
          }
          break;
        }
      }
    }
  }

 private:
  struct udev *udev_ctx_ = nullptr;
  struct udev_monitor *mon_ = nullptr;
  int mon_fd_ = -1;
};

template class Dispatcher<DispatcherUdev>;

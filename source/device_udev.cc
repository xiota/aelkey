#include "device_udev.h"

#include <iostream>
#include <string>

#include <libudev.h>
#include <sol/sol.hpp>
#include <sys/epoll.h>

#include "aelkey_state.h"
#include "device_input.h"
#include "tick_scheduler.h"

void ensure_udev_initialized(sol::this_state ts) {
  if (aelkey_state.epfd >= 0) {
    return;
  }

  sol::state_view lua(ts);

  int epfd = epoll_create1(0);
  if (epfd < 0) {
    throw sol::error("epoll_create1 failed");
  }
  aelkey_state.epfd = epfd;

  aelkey_state.scheduler = new TickScheduler(epfd, lua);

  aelkey_state.g_udev = udev_new();
  if (!aelkey_state.g_udev) {
    throw sol::error("udev_new failed");
  }

  aelkey_state.g_mon = udev_monitor_new_from_netlink(aelkey_state.g_udev, "udev");
  if (!aelkey_state.g_mon) {
    throw sol::error("udev_monitor_new failed");
  }

  udev_monitor_filter_add_match_subsystem_devtype(aelkey_state.g_mon, "input", nullptr);
  udev_monitor_filter_add_match_subsystem_devtype(aelkey_state.g_mon, "hidraw", nullptr);
  udev_monitor_filter_add_match_subsystem_devtype(aelkey_state.g_mon, "usb", nullptr);
  udev_monitor_enable_receiving(aelkey_state.g_mon);

  int mon_fd = udev_monitor_get_fd(aelkey_state.g_mon);
  aelkey_state.udev_fd = mon_fd;

  struct epoll_event ev{};
  ev.events = EPOLLIN;
  ev.data.fd = mon_fd;
  if (epoll_ctl(aelkey_state.epfd, EPOLL_CTL_ADD, mon_fd, &ev) < 0) {
    throw sol::error("epoll_ctl add udev failed");
  }
}

void notify_state_change(sol::this_state ts, const InputDecl &decl, const char *state) {
  if (decl.callback_state.empty()) {
    return;
  }

  sol::state_view lua(ts);

  sol::object obj = lua[decl.callback_state];
  if (!obj.is<sol::function>()) {
    return;
  }

  sol::function cb = obj.as<sol::function>();

  sol::table tbl = lua.create_table();
  tbl["device"] = decl.id;
  tbl["state"] = std::string(state ? state : "");

  sol::protected_function pf = cb;
  sol::protected_function_result result = pf(tbl);
  if (!result.valid()) {
    sol::error err = result;
    std::fprintf(stderr, "Lua state_callback error: %s\n", err.what());
  }
}

void handle_udev_add(sol::this_state ts, struct udev_device *dev) {
  const char *subsystem = udev_device_get_subsystem(dev);
  const char *node = udev_device_get_devnode(dev);
  std::string devnode = node ? node : "";

  if (!subsystem) {
    return;
  }

  // watchlist, notify only
  for (auto &entry : aelkey_state.watch_map) {
    for (auto &decl : entry.second) {
      std::string matched = match_device(decl);

      if ((decl.type == "evdev" && std::string(subsystem) == "input") ||
          (decl.type == "hidraw" && std::string(subsystem) == "hidraw")) {
        if (matched == devnode) {
          decl.devnode = devnode;
          decl.callback_state = aelkey_state.callback_watchlist;
          notify_state_change(ts, decl, "add");
        }
      } else if (decl.type == "libusb" && std::string(subsystem) == "usb") {
        const char *syspath = udev_device_get_syspath(dev);
        if (!syspath) {
          continue;
        }

        if (matched == std::string(syspath)) {
          decl.devnode = syspath;
          decl.callback_state = aelkey_state.callback_watchlist;
          notify_state_change(ts, decl, "add");
        }
      }
    }
  }

  // normal devices, attach and notify
  for (auto &decl : aelkey_state.input_decls) {
    // For all types, ask match_device to resolve the identifier.
    std::string matched = match_device(decl);

    // evdev / hidraw: compare against devnode from the event.
    if ((decl.type == "evdev" && std::string(subsystem) == "input") ||
        (decl.type == "hidraw" && std::string(subsystem) == "hidraw")) {
      if (matched == devnode) {
        if (aelkey_state.input_map.find(decl.id) != aelkey_state.input_map.end()) {
          std::cout << "Device already attached: " << decl.id << std::endl;
          break;
        }
        if (attach_input_device(devnode, decl)) {
          decl.devnode = devnode;  // cache identifier
          notify_state_change(ts, decl, "add");
        }
        break;
      }
    }
    // libusb: compare against syspath from the event.
    else if (decl.type == "libusb" && std::string(subsystem) == "usb") {
      const char *syspath = udev_device_get_syspath(dev);
      if (!syspath) {
        continue;
      }
      if (matched == std::string(syspath)) {
        if (aelkey_state.input_map.find(decl.id) != aelkey_state.input_map.end()) {
          std::cout << "Device already attached: " << decl.id << std::endl;
          break;
        }
        if (attach_input_device(matched, decl)) {
          decl.devnode = matched;  // cache identifier
          notify_state_change(ts, decl, "add");
        }
        break;
      }
    }
  }
}

void handle_udev_remove(sol::this_state ts, struct udev_device *dev) {
  const char *subsystem = udev_device_get_subsystem(dev);
  const char *node = udev_device_get_devnode(dev);
  std::string devnode = node ? node : "";

  if (!subsystem) {
    return;
  }

  // watchlist, notify only
  for (auto &entry : aelkey_state.watch_map) {
    for (auto &decl : entry.second) {
      if (decl.type == "libusb" && std::string(subsystem) == "usb") {
        const char *syspath = udev_device_get_syspath(dev);
        if (!syspath) {
          continue;
        }

        if (decl.devnode == std::string(syspath)) {
          decl.callback_state = aelkey_state.callback_watchlist;
          notify_state_change(ts, decl, "remove");
          decl.devnode.clear();
        }
      } else if ((decl.type == "evdev" && std::string(subsystem) == "input") ||
                 (decl.type == "hidraw" && std::string(subsystem) == "hidraw")) {
        if (decl.devnode == devnode) {
          decl.callback_state = aelkey_state.callback_watchlist;
          notify_state_change(ts, decl, "remove");
          decl.devnode.clear();
        }
      }
    }
  }

  // normal devices, detach and notify
  for (auto &decl : aelkey_state.input_decls) {
    if (decl.type == "libusb" && std::string(subsystem) == "usb") {
      const char *syspath = udev_device_get_syspath(dev);
      if (!syspath) {
        continue;
      }

      if (decl.devnode == std::string(syspath)) {
        InputDecl removed = detach_input_device(decl.id);
        if (!removed.id.empty()) {
          notify_state_change(ts, removed, "remove");
        }
        break;
      }
    } else if ((decl.type == "evdev" && std::string(subsystem) == "input") ||
               (decl.type == "hidraw" && std::string(subsystem) == "hidraw")) {
      if (decl.devnode == devnode) {
        InputDecl removed = detach_input_device(decl.id);
        if (!removed.id.empty()) {
          notify_state_change(ts, removed, "remove");
        }
        break;
      }
    }
  }
}

#include "device_udev.h"

#include <iostream>
#include <string>

#include <libudev.h>
#include <lua.hpp>
#include <sys/epoll.h>

#include "aelkey_state.h"
#include "device_input.h"

int ensure_udev_initialized(lua_State *L) {
  if (aelkey_state.epfd >= 0) {
    return 0;  // already initialized
  }

  int epfd = epoll_create1(0);
  if (epfd < 0) {
    return luaL_error(L, "epoll_create1 failed");
  }
  aelkey_state.epfd = epfd;
  aelkey_state.scheduler = new TickScheduler(epfd, L);

  aelkey_state.g_udev = udev_new();
  if (!aelkey_state.g_udev) {
    return luaL_error(L, "udev_new failed");
  }

  aelkey_state.g_mon = udev_monitor_new_from_netlink(aelkey_state.g_udev, "udev");
  if (!aelkey_state.g_mon) {
    return luaL_error(L, "udev_monitor_new failed");
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
    return luaL_error(L, "epoll_ctl add udev failed");
  }

  return 0;
}

void notify_state_change(lua_State *L, const InputDecl &decl, const char *state) {
  if (decl.callback_state.empty()) {
    return;
  }

  lua_getglobal(L, decl.callback_state.c_str());
  if (!lua_isfunction(L, -1)) {
    lua_pop(L, 1);
    return;
  }

  lua_newtable(L);

  lua_pushstring(L, decl.id.c_str());
  lua_setfield(L, -2, "device");

  lua_pushstring(L, state);
  lua_setfield(L, -2, "state");

  if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
    std::cerr << "Lua state_callback error: " << lua_tostring(L, -1) << std::endl;
    lua_pop(L, 1);
  }
}

void handle_udev_add(lua_State *L, struct udev_device *dev) {
  const char *subsystem = udev_device_get_subsystem(dev);
  const char *node = udev_device_get_devnode(dev);
  std::string devnode = node ? node : "";

  if (!subsystem) {
    return;
  }

  for (auto &decl : aelkey_state.input_decls) {
    // For all types, ask match_device to resolve the identifier
    std::string matched = match_device(decl);

    // For evdev/hidraw, compare against devnode from the event
    if ((decl.type == "evdev" && std::string(subsystem) == "input") ||
        (decl.type == "hidraw" && std::string(subsystem) == "hidraw")) {
      if (matched == devnode) {
        if (aelkey_state.input_map.find(decl.id) != aelkey_state.input_map.end()) {
          std::cout << "Device already attached: " << decl.id << std::endl;
          break;
        }
        if (attach_input_device(devnode, decl)) {
          decl.devnode = devnode;  // cache identifier
          notify_state_change(L, decl, "add");
        }
        break;
      }
    }
    // For libusb, compare against syspath from the event
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
          notify_state_change(L, decl, "add");
        }
        break;
      }
    }
  }
}

void handle_udev_remove(lua_State *L, struct udev_device *dev) {
  const char *subsystem = udev_device_get_subsystem(dev);
  const char *node = udev_device_get_devnode(dev);
  std::string devnode = node ? node : "";

  if (!subsystem) {
    return;
  }

  for (auto &decl : aelkey_state.input_decls) {
    if (decl.type == "libusb" && std::string(subsystem) == "usb") {
      const char *syspath = udev_device_get_syspath(dev);
      if (!syspath) {
        continue;
      }

      if (decl.devnode == std::string(syspath)) {
        InputDecl removed = detach_input_device(decl.id);
        if (!removed.id.empty()) {
          notify_state_change(L, removed, "remove");
        }
        break;
      }
    } else if ((decl.type == "evdev" && std::string(subsystem) == "input") ||
               (decl.type == "hidraw" && std::string(subsystem) == "hidraw")) {
      if (decl.devnode == devnode) {
        InputDecl removed = detach_input_device(decl.id);
        if (!removed.id.empty()) {
          notify_state_change(L, removed, "remove");
        }
        break;
      }
    }
  }
}

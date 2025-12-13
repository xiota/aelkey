#include "device_udev.h"

#include <iostream>
#include <string>

#include <libudev.h>
#include <lua.hpp>
#include <sys/epoll.h>

#include "aelkey_state.h"
#include "device_input.h"

int attach_input_device(const std::string &devnode, const InputDecl &decl) {
  // already attached
  if (aelkey_state.devnode_to_fd.find(devnode) != aelkey_state.devnode_to_fd.end()) {
    std::cout << "Device already attached: " << devnode << std::endl;
    return -1;
  }

  // try to attach
  int newfd = attach_device(
      devnode, decl, aelkey_state.input_map, aelkey_state.frames, aelkey_state.epfd
  );
  if (newfd >= 0) {
    aelkey_state.devnode_to_fd[devnode] = newfd;
    return newfd;
  } else {
    std::cerr << "Failed to attach input: " << decl.id << " (" << devnode << ")" << std::endl;
    return -1;
  }
}

int device_udev_init(lua_State *L) {
  if (aelkey_state.epfd >= 0) {
    return 0;  // already initialized
  }

  int epfd = epoll_create1(0);
  if (epfd < 0) {
    return luaL_error(L, "epoll_create1 failed");
  }
  aelkey_state.epfd = epfd;

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

  lua_pushstring(L, "device");
  lua_pushstring(L, decl.id.c_str());
  lua_settable(L, -3);

  lua_pushstring(L, "state");
  lua_pushstring(L, state);
  lua_settable(L, -3);

  if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
    std::cerr << "Lua state_callback error: " << lua_tostring(L, -1) << std::endl;
    lua_pop(L, 1);
  }
}

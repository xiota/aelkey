#include "device_udev.h"

#include <libudev.h>
#include <lua.hpp>
#include <sys/epoll.h>

#include "aelkey_state.h"

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

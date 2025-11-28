#include <iostream>

#include <CLI/CLI.hpp>
#include <fcntl.h>
#include <libevdev/libevdev.h>
#include <libudev.h>
#include <lua.hpp>
#include <unistd.h>

#include "config.h"

int main(int argc, char **argv) {
  CLI::App app{ "Ælkey Remapper" };
  app.set_version_flag("-V,--version", std::string("aelkey ") + VERSION);
  CLI11_PARSE(app, argc, argv);

  // --- LuaJIT bootstrap ---
  lua_State *L = luaL_newstate();
  luaL_openlibs(L);

  // Run a trivial Lua snippet
  if (luaL_dostring(L, "print('Hello from LuaJIT!')")) {
    std::cerr << "Lua error: " << lua_tostring(L, -1) << std::endl;
    lua_pop(L, 1);
  }

  // --- libevdev: open a device ---
  const char *devnode = "/dev/input/event0";  // adjust to a real device
  int fd = open(devnode, O_RDONLY | O_NONBLOCK);
  if (fd < 0) {
    perror("open");
    return 1;
  }

  struct libevdev *dev = nullptr;
  if (libevdev_new_from_fd(fd, &dev) < 0) {
    std::cerr << "Failed to init libevdev" << std::endl;
    return 1;
  }

  std::cout << "Input device name: " << libevdev_get_name(dev) << std::endl;

  // Read one event (nonblocking)
  struct input_event ev;
  int rc = libevdev_next_event(dev, LIBEVDEV_READ_FLAG_NORMAL, &ev);
  if (rc == 0) {
    std::cout << "Event type=" << ev.type << " code=" << ev.code << " value=" << ev.value
              << std::endl;
  }

  // --- libudev: monitor hotplug ---
  struct udev *udev = udev_new();
  struct udev_monitor *mon = udev_monitor_new_from_netlink(udev, "udev");
  udev_monitor_filter_add_match_subsystem_devtype(mon, "input", nullptr);
  udev_monitor_enable_receiving(mon);

  int mon_fd = udev_monitor_get_fd(mon);
  std::cout << "Listening for udev events..." << std::endl;

  // Poll once for demo
  fd_set fds;
  FD_ZERO(&fds);
  FD_SET(mon_fd, &fds);
  struct timeval tv = { 5, 0 };  // 5s timeout
  if (select(mon_fd + 1, &fds, nullptr, nullptr, &tv) > 0) {
    struct udev_device *dev_event = udev_monitor_receive_device(mon);
    if (dev_event) {
      std::cout << "Udev event: " << udev_device_get_action(dev_event) << " "
                << udev_device_get_devnode(dev_event) << std::endl;
      udev_device_unref(dev_event);
    }
  }

  // Cleanup
  udev_monitor_unref(mon);
  udev_unref(udev);
  libevdev_free(dev);
  close(fd);
  lua_close(L);

  return 0;
}

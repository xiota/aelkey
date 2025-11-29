#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include <CLI/CLI.hpp>
#include <fcntl.h>
#include <libevdev/libevdev.h>
#include <libudev.h>
#include <lua.hpp>
#include <unistd.h>

#include "config.h"

// Recursive dump of a Lua table at the top of the stack
void dump_table_recursive(lua_State *L, int indent = 2) {
  lua_pushnil(L);  // first key
  while (lua_next(L, -2)) {
    // Handle key type
    std::string key;
    if (lua_type(L, -2) == LUA_TSTRING) {
      key = lua_tostring(L, -2);
    } else if (lua_type(L, -2) == LUA_TNUMBER) {
      key = std::to_string(lua_tointeger(L, -2));
    } else {
      key = "(unknown key)";
    }

    if (lua_istable(L, -1)) {
      std::cout << std::string(indent, ' ') << key << " = {\n";
      dump_table_recursive(L, indent + 2);
      std::cout << std::string(indent, ' ') << "}\n";
    } else if (lua_isstring(L, -1)) {
      std::cout << std::string(indent, ' ') << key << " = " << lua_tostring(L, -1) << "\n";
    } else if (lua_isnumber(L, -1)) {
      std::cout << std::string(indent, ' ') << key << " = " << lua_tonumber(L, -1) << "\n";
    } else if (lua_isboolean(L, -1)) {
      std::cout << std::string(indent, ' ') << key << " = "
                << (lua_toboolean(L, -1) ? "true" : "false") << "\n";
    } else {
      std::cout << std::string(indent, ' ') << key << " = (unsupported type)\n";
    }
    lua_pop(L, 1);  // pop value, keep key
  }
}

int main(int argc, char **argv) {
  CLI::App app{ "Ælkey Remapper" };
  app.set_version_flag("-V,--version", std::string("aelkey ") + VERSION);

  // Accept multiple files/folders as positional arguments
  std::vector<std::string> paths;
  app.add_option("paths", paths, "Lua script files or directories")
      ->expected(-1);  // unlimited arguments

  CLI11_PARSE(app, argc, argv);

  // --- Collect Lua scripts ---
  std::vector<std::string> lua_scripts;

  if (paths.empty()) {
    paths.push_back("/etc/aelkey");
  }

  auto is_lua_file = [](const std::filesystem::path &p) {
    return std::filesystem::is_regular_file(p) && p.extension() == ".lua";
  };

  for (const auto &p : paths) {
    std::filesystem::path path(p);
    if (is_lua_file(path)) {
      lua_scripts.push_back(path.string());
    } else if (std::filesystem::is_directory(path)) {
      for (const auto &entry : std::filesystem::directory_iterator(path)) {
        if (is_lua_file(entry.path())) {
          lua_scripts.push_back(entry.path().string());
        }
      }
    }
  }

  // --- LuaJIT bootstrap ---
  lua_State *L = luaL_newstate();
  luaL_openlibs(L);

  if (!lua_scripts.empty()) {
    const std::string &first_script = lua_scripts.front();
    std::cout << "Running Lua script: " << first_script << std::endl;
    if (luaL_dofile(L, first_script.c_str())) {
      std::cerr << "Lua error: " << lua_tostring(L, -1) << std::endl;
      lua_pop(L, 1);
    }
  }

  // --- Metadata parsing ---
  auto dump_global_table = [&](const char *global_name) {
    lua_getglobal(L, global_name);
    if (!lua_istable(L, -1)) {
      std::cout << global_name << " is not defined or not a table\n";
      lua_pop(L, 1);
      return;
    }

    std::cout << "=== " << global_name << " ===\n";
    dump_table_recursive(L);
    lua_pop(L, 1);  // pop global table
  };

  dump_global_table("inputs");
  dump_global_table("output_devices");

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

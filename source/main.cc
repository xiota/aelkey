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

struct InputDecl {
  std::string id;
  std::string kind;
  int vendor = 0;
  int product = 0;
  bool writable = false;
};

struct OutputDecl {
  std::string id;
  std::string type;
  std::string name;
};

// Parse one input entry { id=..., kind=..., match={...}, writable=... }
InputDecl parse_input(lua_State *L, int index) {
  InputDecl decl;
  lua_pushnil(L);
  while (lua_next(L, index)) {
    std::string key = lua_tostring(L, -2);
    if (key == "id" && lua_isstring(L, -1)) {
      decl.id = lua_tostring(L, -1);
    } else if (key == "kind" && lua_isstring(L, -1)) {
      decl.kind = lua_tostring(L, -1);
    } else if (key == "writable" && lua_isboolean(L, -1)) {
      decl.writable = lua_toboolean(L, -1);
    } else if (key == "match" && lua_istable(L, -1)) {
      lua_pushnil(L);
      while (lua_next(L, -2)) {
        std::string mkey = lua_tostring(L, -2);
        if (mkey == "vendor" && lua_isnumber(L, -1)) {
          decl.vendor = (int)lua_tointeger(L, -1);
        } else if (mkey == "product" && lua_isnumber(L, -1)) {
          decl.product = (int)lua_tointeger(L, -1);
        }
        lua_pop(L, 1);
      }
    }
    lua_pop(L, 1);
  }
  return decl;
}

// Parse one output entry { id=..., type=..., name=... }
OutputDecl parse_output(lua_State *L, int index) {
  OutputDecl decl;
  lua_pushnil(L);
  while (lua_next(L, index)) {
    std::string key = lua_tostring(L, -2);
    if (key == "id" && lua_isstring(L, -1)) {
      decl.id = lua_tostring(L, -1);
    } else if (key == "type" && lua_isstring(L, -1)) {
      decl.type = lua_tostring(L, -1);
    } else if (key == "name" && lua_isstring(L, -1)) {
      decl.name = lua_tostring(L, -1);
    }
    lua_pop(L, 1);
  }
  return decl;
}

// Try to match one InputDecl against available /dev/input/event* devices
std::string match_device(const InputDecl &decl) {
  for (int i = 0;; i++) {
    std::string devnode = "/dev/input/event" + std::to_string(i);
    int fd = open(devnode.c_str(), O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
      // stop when no more devices
      break;
    }

    struct libevdev *dev = nullptr;
    if (libevdev_new_from_fd(fd, &dev) == 0) {
      int vendor = libevdev_get_id_vendor(dev);
      int product = libevdev_get_id_product(dev);

      if (vendor == decl.vendor && product == decl.product) {
        std::cout << "Matched " << decl.id << " → " << devnode << " (" << libevdev_get_name(dev)
                  << ")\n";
        libevdev_free(dev);
        close(fd);
        return devnode;  // return the matching path
      }
    }
    libevdev_free(dev);
    close(fd);
  }
  return {};
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
  std::vector<InputDecl> inputs;
  std::vector<OutputDecl> outputs;

  // Parse inputs
  lua_getglobal(L, "inputs");
  if (lua_istable(L, -1)) {
    int len = lua_objlen(L, -1);
    for (int i = 1; i <= len; i++) {
      lua_rawgeti(L, -1, i);
      if (lua_istable(L, -1)) {
        inputs.push_back(parse_input(L, lua_gettop(L)));
      }
      lua_pop(L, 1);
    }
  }
  lua_pop(L, 1);

  // Parse outputs
  lua_getglobal(L, "output_devices");
  if (lua_istable(L, -1)) {
    int len = lua_objlen(L, -1);
    for (int i = 1; i <= len; i++) {
      lua_rawgeti(L, -1, i);
      if (lua_istable(L, -1)) {
        outputs.push_back(parse_output(L, lua_gettop(L)));
      }
      lua_pop(L, 1);
    }
  }
  lua_pop(L, 1);

  // --- libevdev: open devices based on Lua metadata ---
  int fd = -1;
  struct libevdev *dev = nullptr;

  for (auto &in : inputs) {
    std::string devnode = match_device(in);
    if (!devnode.empty()) {
      fd = open(devnode.c_str(), O_RDONLY | O_NONBLOCK);
      if (fd < 0) {
        perror("open");
        continue;
      }
      if (libevdev_new_from_fd(fd, &dev) < 0) {
        std::cerr << "Failed to init libevdev for " << devnode << std::endl;
        close(fd);
        continue;
      }
      std::cout << "Input device name: " << libevdev_get_name(dev) << std::endl;

      // Continuous event loop (demo)
      while (true) {
        struct input_event ev;
        int rc = libevdev_next_event(dev, LIBEVDEV_READ_FLAG_NORMAL, &ev);

        if (rc == 0) {
          // Push the remap function
          lua_getglobal(L, "remap");
          if (lua_isfunction(L, -1)) {
            lua_newtable(L);
            lua_pushstring(L, "type");
            lua_pushinteger(L, ev.type);
            lua_settable(L, -3);

            lua_pushstring(L, "code");
            lua_pushinteger(L, ev.code);
            lua_settable(L, -3);

            lua_pushstring(L, "value");
            lua_pushinteger(L, ev.value);
            lua_settable(L, -3);

            if (lua_pcall(L, 1, 0, 0) != 0) {
              std::cerr << "Lua error: " << lua_tostring(L, -1) << std::endl;
              lua_pop(L, 1);
            }
          } else {
            lua_pop(L, 1);  // remove non-function
          }
        }
      }
    }
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
  lua_close(L);

  if (dev) {
    libevdev_free(dev);
  }
  if (fd >= 0) {
    close(fd);
  }

  return 0;
}

#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include <CLI/CLI.hpp>
#include <fcntl.h>
#include <libevdev/libevdev-uinput.h>
#include <libevdev/libevdev.h>
#include <libudev.h>
#include <lua.hpp>
#include <sys/epoll.h>
#include <unistd.h>

#include "config.h"
#include "util/scoped_timer.h"

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

std::unordered_map<std::string, libevdev_uinput *> uinput_devices;

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

struct libevdev_uinput *create_output_device(const OutputDecl &out) {
  struct libevdev *dev = libevdev_new();
  libevdev_set_name(dev, out.name.c_str());

  if (out.type == "keyboard") {
    libevdev_enable_event_type(dev, EV_KEY);
    // Enable a few demo keys; expand later
    libevdev_enable_event_code(dev, EV_KEY, KEY_A, nullptr);
    libevdev_enable_event_code(dev, EV_KEY, KEY_B, nullptr);
    libevdev_enable_event_code(dev, EV_KEY, KEY_C, nullptr);
  } else if (out.type == "mouse") {
    libevdev_enable_event_type(dev, EV_KEY);
    libevdev_enable_event_code(dev, EV_KEY, BTN_LEFT, nullptr);
    libevdev_enable_event_code(dev, EV_KEY, BTN_RIGHT, nullptr);
    libevdev_enable_event_code(dev, EV_KEY, BTN_MIDDLE, nullptr);
    libevdev_enable_event_code(dev, EV_KEY, BTN_SIDE, nullptr);
    libevdev_enable_event_code(dev, EV_KEY, BTN_EXTRA, nullptr);
    libevdev_enable_event_code(dev, EV_KEY, BTN_FORWARD, nullptr);
    libevdev_enable_event_code(dev, EV_KEY, BTN_BACK, nullptr);

    libevdev_enable_event_type(dev, EV_REL);
    libevdev_enable_event_code(dev, EV_REL, REL_X, nullptr);
    libevdev_enable_event_code(dev, EV_REL, REL_Y, nullptr);
    libevdev_enable_event_code(dev, EV_REL, REL_WHEEL, nullptr);
    libevdev_enable_event_code(dev, EV_REL, REL_HWHEEL, nullptr);
    libevdev_enable_event_code(dev, EV_REL, REL_WHEEL_HI_RES, nullptr);
    libevdev_enable_event_code(dev, EV_REL, REL_HWHEEL_HI_RES, nullptr);
  }

  struct libevdev_uinput *uidev = nullptr;
  int err = libevdev_uinput_create_from_device(dev, LIBEVDEV_UINPUT_OPEN_MANAGED, &uidev);
  if (err != 0) {
    std::cerr << "Failed to create uinput device: " << out.name << std::endl;
    libevdev_free(dev);
    return nullptr;
  }

  std::cout << "Created uinput device: " << out.name << " at "
            << libevdev_uinput_get_devnode(uidev) << std::endl;

  libevdev_free(dev);  // description no longer needed
  return uidev;
}

// Lua binding: emit({ device=..., type=..., code=..., value=... })
int lua_emit(lua_State *L) {
  luaL_checktype(L, 1, LUA_TTABLE);

  const char *dev_id = nullptr;
  int type = 0, code = 0, value = 0;

  lua_getfield(L, 1, "device");
  if (lua_isstring(L, -1)) {
    dev_id = lua_tostring(L, -1);
  }
  lua_pop(L, 1);

  lua_getfield(L, 1, "type");
  type = luaL_checkinteger(L, -1);
  lua_pop(L, 1);

  lua_getfield(L, 1, "code");
  code = luaL_checkinteger(L, -1);
  lua_pop(L, 1);

  lua_getfield(L, 1, "value");
  value = luaL_checkinteger(L, -1);
  lua_pop(L, 1);

  if (!dev_id) {
    if (uinput_devices.size() == 1) {
      auto it = uinput_devices.begin();
      libevdev_uinput_write_event(it->second, type, code, value);
    } else {
      return luaL_error(L, "emit requires 'device' when multiple output devices are present");
    }
  } else {
    auto it = uinput_devices.find(dev_id);
    if (it == uinput_devices.end()) {
      return luaL_error(L, "Unknown device id: %s", dev_id);
    }
    libevdev_uinput_write_event(it->second, type, code, value);
  }
  return 0;
}

int lua_syn_report(lua_State *L) {
  const char *dev_id = luaL_optstring(L, 1, nullptr);  // optional device name
  if (dev_id) {
    // look up matching uinput device by id
    auto it = uinput_devices.find(dev_id);
    if (it != uinput_devices.end()) {
      libevdev_uinput_write_event(it->second, EV_SYN, SYN_REPORT, 0);
    }
  } else {
    // flush all devices (initial simple policy)
    for (auto &kv : uinput_devices) {
      libevdev_uinput_write_event(kv.second, EV_SYN, SYN_REPORT, 0);
    }
  }
  return 0;
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

  lua_register(L, "emit", lua_emit);
  lua_register(L, "syn_report", lua_syn_report);

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

  for (auto &out : outputs) {
    libevdev_uinput *uidev = create_output_device(out);
    if (uidev) {
      uinput_devices[out.id] = uidev;
    }
  }

  // --- create epoll instance ---
  int epfd = epoll_create1(0);
  if (epfd < 0) {
    perror("epoll_create1");
    return 1;
  }

  // --- libevdev: open devices based on Lua metadata ---
  int fd = -1;
  struct libevdev *dev = nullptr;

  // Map fds to their InputDecl/libevdev and keep per‑device frame
  std::unordered_map<int, std::pair<InputDecl, libevdev *>> input_map;
  std::unordered_map<int, std::vector<struct input_event>> frames;

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

      input_map[fd] = { in, dev };
      frames[fd] = {};

      // Register input fd with epoll
      struct epoll_event evreg;
      evreg.events = EPOLLIN;
      evreg.data.fd = fd;
      epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &evreg);
    }
  }

  // --- libudev: monitor hotplug ---
  struct udev *udev = udev_new();
  struct udev_monitor *mon = udev_monitor_new_from_netlink(udev, "udev");
  udev_monitor_filter_add_match_subsystem_devtype(mon, "input", nullptr);
  udev_monitor_enable_receiving(mon);
  int mon_fd = udev_monitor_get_fd(mon);

  // Register udev fd with epoll
  struct epoll_event evreg;
  evreg.events = EPOLLIN;
  evreg.data.fd = mon_fd;
  epoll_ctl(epfd, EPOLL_CTL_ADD, mon_fd, &evreg);

  // Unified epoll loop
  struct epoll_event events[16];
  while (true) {
    int n = epoll_wait(epfd, events, 16, -1);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      perror("epoll_wait");
      break;
    }
    for (int i = 0; i < n; i++) {
      int fd_ready = events[i].data.fd;
      if (fd_ready == mon_fd) {
        struct udev_device *dev_event = udev_monitor_receive_device(mon);
        if (dev_event) {
          std::cout << "Udev event: " << udev_device_get_action(dev_event) << " "
                    << udev_device_get_devnode(dev_event) << std::endl;
          udev_device_unref(dev_event);
        }
      } else {
        // Input device event
        auto &[in, idev] = input_map[fd_ready];
        struct input_event ev;
        auto &frame = frames[fd_ready];  // reuse per‑device frame
        while (libevdev_next_event(idev, LIBEVDEV_READ_FLAG_NORMAL, &ev) ==
               LIBEVDEV_READ_STATUS_SUCCESS) {
          frame.push_back(ev);
          if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
            lua_getglobal(L, "remap");
            if (lua_isfunction(L, -1)) {
              lua_newtable(L);
              for (size_t j = 0; j < frame.size(); ++j) {
                lua_pushinteger(L, (int)j + 1);
                lua_newtable(L);

                lua_pushstring(L, "device");
                lua_pushstring(L, in.id.c_str());
                lua_settable(L, -3);

                lua_pushstring(L, "type");
                lua_pushinteger(L, frame[j].type);
                lua_settable(L, -3);

                lua_pushstring(L, "code");
                lua_pushinteger(L, frame[j].code);
                lua_settable(L, -3);

                lua_pushstring(L, "value");
                lua_pushinteger(L, frame[j].value);
                lua_settable(L, -3);

                lua_settable(L, -3);
              }
              PROFILE_CALL("Lua remap", {
                if (lua_pcall(L, 1, 0, 0) != 0) {
                  std::cerr << "Lua error: " << lua_tostring(L, -1) << std::endl;
                  lua_pop(L, 1);
                }
              });
            } else {
              lua_pop(L, 1);
            }
            frame.clear();
          }
        }
      }
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

  for (auto &[id, uidev] : uinput_devices) {
    libevdev_uinput_destroy(uidev);
  }

  return 0;
}

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
#include <sys/timerfd.h>
#include <unistd.h>

#include "config.h"
#include "util/scoped_timer.h"

struct InputDecl {
  std::string id;
  std::string kind;
  int vendor = 0;
  int product = 0;
  int bus = 0;
  std::string name;
  std::string phys;
  std::string uniq;
  bool writable = false;
  bool grab = false;
};

struct OutputDecl {
  std::string id;
  std::string type;
  std::string name;
};

std::unordered_map<std::string, libevdev_uinput *> uinput_devices;

static int tfd = -1;
static int epfd = -1;

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
    } else if (key == "grab" && lua_isboolean(L, -1)) {
      decl.grab = lua_toboolean(L, -1);
    } else if (key == "match" && lua_istable(L, -1)) {
      lua_pushnil(L);
      while (lua_next(L, -2)) {
        std::string mkey = lua_tostring(L, -2);
        if (mkey == "vendor" && lua_isnumber(L, -1)) {
          decl.vendor = (int)lua_tointeger(L, -1);
        } else if (mkey == "product" && lua_isnumber(L, -1)) {
          decl.product = (int)lua_tointeger(L, -1);
        } else if (mkey == "bus" && lua_isstring(L, -1)) {
          std::string busstr = lua_tostring(L, -1);
          if (busstr == "usb") {
            decl.bus = BUS_USB;
          } else if (busstr == "bluetooth") {
            decl.bus = BUS_BLUETOOTH;
          } else if (busstr == "pci") {
            decl.bus = BUS_PCI;
          }
          // add more as needed
        } else if (mkey == "name" && lua_isstring(L, -1)) {
          decl.name = lua_tostring(L, -1);
        } else if (mkey == "phys" && lua_isstring(L, -1)) {
          decl.phys = lua_tostring(L, -1);
        } else if (mkey == "uniq" && lua_isstring(L, -1)) {
          decl.uniq = lua_tostring(L, -1);
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
      break;
    }

    struct libevdev *dev = nullptr;
    if (libevdev_new_from_fd(fd, &dev) == 0) {
      int bus = libevdev_get_id_bustype(dev);
      int vendor = libevdev_get_id_vendor(dev);
      int product = libevdev_get_id_product(dev);
      const char *name = libevdev_get_name(dev);
      const char *phys = libevdev_get_phys(dev);
      const char *uniq = libevdev_get_uniq(dev);

      bool match = true;
      if (decl.bus && bus != decl.bus) {
        match = false;
      }
      if (decl.vendor && vendor != decl.vendor) {
        match = false;
      }
      if (decl.product && product != decl.product) {
        match = false;
      }
      if (!decl.name.empty() && decl.name != (name ? name : "")) {
        match = false;
      }
      if (!decl.phys.empty() && decl.phys != (phys ? phys : "")) {
        match = false;
      }
      if (!decl.uniq.empty() && decl.uniq != (uniq ? uniq : "")) {
        match = false;
      }

      if (match) {
        std::cout << "Matched " << decl.id << " → " << devnode << " (" << (name ? name : "")
                  << ")\n";
        libevdev_free(dev);
        close(fd);
        return devnode;
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

int lua_tick(lua_State *L) {
  int ms = luaL_checkinteger(L, 1);

  if (tfd != -1) {
    close(tfd);
  }

  tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
  if (tfd < 0) {
    perror("timerfd_create");
    return 0;
  }

  struct itimerspec spec{};
  spec.it_interval.tv_sec = ms / 1000;
  spec.it_interval.tv_nsec = (ms % 1000) * 1000000;
  spec.it_value = spec.it_interval;

  if (timerfd_settime(tfd, 0, &spec, nullptr) < 0) {
    perror("timerfd_settime");
    return 0;
  }

  struct epoll_event ev{};
  ev.events = EPOLLIN;
  ev.data.fd = tfd;
  epoll_ctl(epfd, EPOLL_CTL_ADD, tfd, &ev);

  return 0;
}

// Attach a device using the same metadata (InputDecl) rules.
// Returns the opened fd on success, or -1 on failure.
int attach_device(
    const std::string &devnode,
    const InputDecl &in,
    std::unordered_map<int, std::pair<InputDecl, libevdev *>> &input_map,
    std::unordered_map<int, std::vector<struct input_event>> &frames,
    int epfd
) {
  int fd = open(devnode.c_str(), O_RDONLY | O_NONBLOCK);
  if (fd < 0) {
    perror("open");
    return -1;
  }

  struct libevdev *idev = nullptr;
  if (libevdev_new_from_fd(fd, &idev) < 0) {
    std::cerr << "Failed to init libevdev for " << devnode << std::endl;
    close(fd);
    return -1;
  }

  std::cout << "Attached input device: " << libevdev_get_name(idev) << std::endl;

  if (in.grab) {
    int rc = libevdev_grab(idev, LIBEVDEV_GRAB);
    if (rc < 0) {
      std::cerr << "Failed to grab device " << devnode << ": " << strerror(-rc) << std::endl;
    } else {
      std::cout << "Grabbed device exclusively: " << devnode << std::endl;
    }
  }

  input_map[fd] = { in, idev };
  frames[fd] = {};

  struct epoll_event evreg{};
  evreg.events = EPOLLIN;
  evreg.data.fd = fd;
  if (epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &evreg) < 0) {
    perror("epoll_ctl EPOLL_CTL_ADD");
    libevdev_grab(idev, LIBEVDEV_UNGRAB);
    libevdev_free(idev);
    input_map.erase(fd);
    frames.erase(fd);
    close(fd);
    return -1;
  }

  return fd;
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
  lua_register(L, "tick", lua_tick);

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
  epfd = epoll_create1(0);
  if (epfd < 0) {
    perror("epoll_create1");
    return 1;
  }

  // Map fds to their InputDecl/libevdev and keep per‑device frame
  std::unordered_map<int, std::pair<InputDecl, libevdev *>> input_map;
  std::unordered_map<int, std::vector<struct input_event>> frames;

  // Track devnode strings so we can cleanup by devnode on udev remove
  std::unordered_map<std::string, int> devnode_to_fd;

  for (auto &in : inputs) {
    std::string devnode = match_device(in);
    if (!devnode.empty()) {
      // Attach using the same path used for reconnects
      int newfd = attach_device(devnode, in, input_map, frames, epfd);
      if (newfd >= 0) {
        devnode_to_fd[devnode] = newfd;
      } else {
        std::cerr << "Failed to attach input: " << in.id << " (" << devnode << ")" << std::endl;
      }
    }
  }

  // --- libudev: monitor hotplug ---
  struct udev *udev = udev_new();
  struct udev_monitor *mon = udev_monitor_new_from_netlink(udev, "udev");
  udev_monitor_filter_add_match_subsystem_devtype(mon, "input", nullptr);
  udev_monitor_enable_receiving(mon);
  int mon_fd = udev_monitor_get_fd(mon);

  // Register udev fd with epoll
  struct epoll_event evreg{};
  evreg.events = EPOLLIN;
  evreg.data.fd = mon_fd;
  if (epoll_ctl(epfd, EPOLL_CTL_ADD, mon_fd, &evreg) < 0) {
    perror("epoll_ctl EPOLL_CTL_ADD (udev)");
    udev_monitor_unref(mon);
    udev_unref(udev);
    return 1;
  }

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
          const char *action_c = udev_device_get_action(dev_event);
          const char *devnode_c = udev_device_get_devnode(dev_event);
          std::string action = action_c ? action_c : "";
          std::string devnode = devnode_c ? devnode_c : "";
          std::cout << "Udev event: " << action << " " << devnode << std::endl;

          if (action == "remove") {
            // cleanup: find fd by devnode, close, free libevdev, erase from maps
            if (!devnode.empty()) {
              auto it = devnode_to_fd.find(devnode);
              if (it != devnode_to_fd.end()) {
                int dfd = it->second;
                // Remove from epoll first
                if (epoll_ctl(epfd, EPOLL_CTL_DEL, dfd, nullptr) < 0) {
                  perror("epoll_ctl EPOLL_CTL_DEL");
                }
                // Free libevdev and close fd
                auto im = input_map.find(dfd);
                if (im != input_map.end()) {
                  libevdev *idev = im->second.second;
                  if (idev) {
                    libevdev_grab(idev, LIBEVDEV_UNGRAB);
                    libevdev_free(idev);
                  }
                  input_map.erase(im);
                }
                frames.erase(dfd);
                close(dfd);
                devnode_to_fd.erase(it);
              }
            }
          } else if (action == "add") {
            // Reattach using the same matching logic as startup
            if (!devnode.empty()) {
              for (auto &decl : inputs) {
                std::string candidate = match_device(decl);
                if (candidate == devnode) {
                  int newfd = attach_device(devnode, decl, input_map, frames, epfd);
                  if (newfd >= 0) {
                    devnode_to_fd[devnode] = newfd;
                  }
                  break;
                }
              }
            }
          }

          udev_device_unref(dev_event);
        }
      } else if (tfd >= 0 && fd_ready == tfd) {
        uint64_t expirations;
        read(tfd, &expirations, sizeof(expirations));

        lua_getglobal(L, "remap");
        if (lua_isfunction(L, -1)) {
          lua_newtable(L);
          lua_pushinteger(L, 1);
          lua_newtable(L);

          lua_pushstring(L, "device");
          lua_pushstring(L, "tick");
          lua_settable(L, -3);

          lua_pushstring(L, "type");
          lua_pushinteger(L, 0);
          lua_settable(L, -3);

          lua_pushstring(L, "code");
          lua_pushinteger(L, 0);
          lua_settable(L, -3);

          lua_pushstring(L, "value");
          lua_pushinteger(L, (int)expirations);
          lua_settable(L, -3);

          lua_settable(L, -3);

          if (lua_pcall(L, 1, 0, 0) != 0) {
            std::cerr << "Lua error: " << lua_tostring(L, -1) << std::endl;
            lua_pop(L, 1);
          }
        } else {
          lua_pop(L, 1);
        }
      } else {
        // Input device event
        // If the kernel signaled a hangup or error, proactively cleanup this fd.
        // Also skip processing if the fd isn't EPOLLIN (defensive).
        if ((events[i].events & EPOLLIN) == 0 &&
            (events[i].events & (EPOLLHUP | EPOLLERR)) == 0) {
          continue;
        }
        if ((events[i].events & (EPOLLHUP | EPOLLERR)) != 0) {
          auto it_fd = devnode_to_fd.begin();
          // Find devnode for this fd (reverse lookup).
          for (; it_fd != devnode_to_fd.end(); ++it_fd) {
            if (it_fd->second == fd_ready) {
              break;
            }
          }
          // Remove from epoll and maps safely.
          if (epoll_ctl(epfd, EPOLL_CTL_DEL, fd_ready, nullptr) < 0) {
            perror("epoll_ctl EPOLL_CTL_DEL (HUP/ERR)");
          }
          auto im = input_map.find(fd_ready);
          if (im != input_map.end()) {
            libevdev *idev = im->second.second;
            if (idev) {
              libevdev_grab(idev, LIBEVDEV_UNGRAB);
              libevdev_free(idev);
            }
            input_map.erase(im);
          }
          frames.erase(fd_ready);
          close(fd_ready);
          if (it_fd != devnode_to_fd.end()) {
            devnode_to_fd.erase(it_fd);
          }
          continue;
        }

        auto it = input_map.find(fd_ready);
        if (it == input_map.end()) {
          // fd was removed; ignore spurious events
          continue;
        }
        auto &[in, idev] = it->second;

        // Guard against null libevdev* in case of a race
        if (!idev) {
          continue;
        }

        struct input_event ev;
        auto fit = frames.find(fd_ready);
        if (fit == frames.end()) {
          continue;
        }
        auto &frame = fit->second;  // reuse per‑device frame
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

  for (auto &[id, uidev] : uinput_devices) {
    libevdev_uinput_destroy(uidev);
  }

  return 0;
}

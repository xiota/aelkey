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
#include "device_input.h"
#include "device_output.h"
#include "globals.h"
#include "lua_bindings.h"
#include "util/scoped_timer.h"

// globals
int tfd = -1;
int epfd = -1;

std::unordered_map<std::string, libevdev_uinput *> uinput_devices;

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
  register_lua_bindings(L);

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
        uint64_t expirations = 0;
        (void)read(tfd, &expirations, sizeof(expirations));

        // Call Lua tock(event)
        lua_getglobal(L, "tock");
        if (lua_isfunction(L, -1)) {
          lua_newtable(L);

          // source: user timer (you can add "fusion" etc. from other triggers)
          lua_pushstring(L, "source");
          lua_pushstring(L, "user");
          lua_settable(L, -3);

          // number of expirations since last read
          lua_pushstring(L, "value");
          lua_pushinteger(L, (int)expirations);
          lua_settable(L, -3);

          // optional payload provided by tick(ms, data)
          extern int tick_payload_ref;
          if (tick_payload_ref != LUA_NOREF) {
            lua_pushstring(L, "payload");
            lua_rawgeti(L, LUA_REGISTRYINDEX, tick_payload_ref);
            lua_settable(L, -3);
          }

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

                lua_pushstring(L, "type_name");
                lua_pushstring(L, libevdev_event_type_get_name(frame[j].type));
                lua_settable(L, -3);

                lua_pushstring(L, "code");
                lua_pushinteger(L, frame[j].code);
                lua_settable(L, -3);

                lua_pushstring(L, "code_name");
                lua_pushstring(L, libevdev_event_code_get_name(frame[j].type, frame[j].code));
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

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
int epfd = -1;

std::unordered_map<std::string, libevdev_uinput *> uinput_devices;
std::unordered_map<int, TickCallback> tick_callbacks;

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

  // Map fds to InputCtx (decl + libevdev + optional callback ref) and keep per‑device frame
  std::unordered_map<int, InputCtx> input_map;
  std::unordered_map<int, std::vector<struct input_event>> frames;

  // Track devnode strings so we can cleanup by devnode on udev remove
  std::unordered_map<std::string, int> devnode_to_fd;

  for (auto &in : inputs) {
    std::string devnode = match_device(in);
    if (!devnode.empty()) {
      // skip if already attached
      if (devnode_to_fd.find(devnode) != devnode_to_fd.end()) {
        std::cout << "Device already attached: " << devnode << std::endl;
        continue;
      }
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
  udev_monitor_filter_add_match_subsystem_devtype(mon, "hidraw", nullptr);
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
                  auto &ctx = im->second;
                  if (ctx.idev) {
                    libevdev_grab(ctx.idev, LIBEVDEV_UNGRAB);
                    libevdev_free(ctx.idev);
                  }
                  input_map.erase(im);
                  frames.erase(dfd);
                }
                close(dfd);
                devnode_to_fd.erase(it);
              }
            }
          } else if (action == "add") {
            // Reattach using the same matching logic as startup
            if (!devnode.empty()) {
              // skip if already attached
              if (devnode_to_fd.find(devnode) != devnode_to_fd.end()) {
                std::cout << "Device already attached: " << devnode << std::endl;
                continue;
              }
              for (auto &decl : inputs) {
                std::string candidate = match_device(decl);
                if (candidate == devnode) {
                  int newfd = attach_device(devnode, decl, input_map, frames, epfd);
                  if (newfd >= 0) {
                    devnode_to_fd[devnode] = newfd;
                  } else {
                    std::cerr << "Failed to reattach input: " << decl.id << " (" << devnode
                              << ")" << std::endl;
                  }
                  break;
                }
              }
            }
          }
          udev_device_unref(dev_event);
        }
        continue;
      } else {
        // --- Tick dispatch: check if this fd belongs to a timer ---
        auto it_tick = tick_callbacks.find(fd_ready);
        if (it_tick != tick_callbacks.end()) {
          uint64_t expirations = 0;
          ssize_t r = read(fd_ready, &expirations, sizeof(expirations));
          if (r > 0) {
            auto &cb = it_tick->second;
            if (cb.is_function && cb.ref != LUA_NOREF) {
              lua_rawgeti(L, LUA_REGISTRYINDEX, cb.ref);
              lua_pushinteger(L, (lua_Integer)expirations);
              if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
                std::cerr << "Lua tick callback error: " << lua_tostring(L, -1) << std::endl;
                lua_pop(L, 1);
              }
            } else if (!cb.name.empty()) {
              lua_getglobal(L, cb.name.c_str());
              if (lua_isfunction(L, -1)) {
                lua_pushinteger(L, (lua_Integer)expirations);
                if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
                  std::cerr << "Lua tick callback error: " << lua_tostring(L, -1) << std::endl;
                  lua_pop(L, 1);
                }
              } else {
                lua_pop(L, 1);  // not a function
              }
            }
          } else if (r < 0 && errno != EAGAIN) {
            perror("read(timerfd)");
          }
          continue;  // handled tick
        }

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
            auto &ctx = im->second;
            if (ctx.idev) {
              libevdev_grab(ctx.idev, LIBEVDEV_UNGRAB);
              libevdev_free(ctx.idev);
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

        auto it_map = input_map.find(fd_ready);
        if (it_map == input_map.end()) {
          // fd was removed; ignore spurious events
          continue;
        }
        auto &ctx = it_map->second;

        if (ctx.decl.kind == "hidraw") {
          uint8_t buf[64];
          ssize_t r = read(fd_ready, buf, sizeof(buf));
          if (r > 0 && !ctx.decl.callback.empty()) {
            lua_getglobal(L, ctx.decl.callback.c_str());
            if (lua_isfunction(L, -1)) {
              lua_newtable(L);
              lua_pushstring(L, "device");
              lua_pushstring(L, ctx.decl.id.c_str());
              lua_settable(L, -3);

              lua_pushstring(L, "report");
              lua_pushlstring(L, (const char *)buf, r);
              lua_settable(L, -3);

              PROFILE_CALL("Lua hidraw", {
                if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
                  std::cerr << "Lua hidraw callback error: " << lua_tostring(L, -1)
                            << std::endl;
                  lua_pop(L, 1);
                }
              });
            } else {
              lua_pop(L, 1);
            }
          }
        } else {
          // default: evdev
          if (!ctx.idev) {
            continue;
          }
          struct input_event ev;
          auto fit = frames.find(fd_ready);
          if (fit == frames.end()) {
            continue;
          }
          auto &frame = fit->second;
          while (libevdev_next_event(ctx.idev, LIBEVDEV_READ_FLAG_NORMAL, &ev) ==
                 LIBEVDEV_READ_STATUS_SUCCESS) {
            frame.push_back(ev);
            if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
              if (!ctx.decl.callback.empty()) {
                lua_getglobal(L, ctx.decl.callback.c_str());
                if (lua_isfunction(L, -1)) {
                  lua_newtable(L);
                  for (size_t j = 0; j < frame.size(); ++j) {
                    const auto &fev = frame[j];
                    lua_pushinteger(L, (int)j + 1);
                    lua_newtable(L);

                    lua_pushstring(L, "device");
                    lua_pushstring(L, ctx.decl.id.c_str());
                    lua_settable(L, -3);

                    lua_pushstring(L, "type");
                    lua_pushinteger(L, fev.type);
                    lua_settable(L, -3);

                    lua_pushstring(L, "type_name");
                    lua_pushstring(L, libevdev_event_type_get_name(fev.type));
                    lua_settable(L, -3);

                    lua_pushstring(L, "code");
                    lua_pushinteger(L, fev.code);
                    lua_settable(L, -3);

                    lua_pushstring(L, "code_name");
                    lua_pushstring(L, libevdev_event_code_get_name(fev.type, fev.code));
                    lua_settable(L, -3);

                    lua_pushstring(L, "value");
                    lua_pushinteger(L, fev.value);
                    lua_settable(L, -3);

                    lua_settable(L, -3);
                  }
                  PROFILE_CALL("Lua evdev", {
                    if (lua_pcall(L, 1, 0, 0) != 0) {
                      std::cerr << "Lua callback error (" << ctx.decl.id
                                << "): " << lua_tostring(L, -1) << std::endl;
                      lua_pop(L, 1);
                    }
                  });
                } else {
                  lua_pop(L, 1);
                }
              }
              frame.clear();
            }
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

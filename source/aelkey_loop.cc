#include "aelkey_core.h"

#include <csignal>
#include <cstring>
#include <iostream>
#include <string>

#include <libevdev/libevdev-uinput.h>
#include <libudev.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include "aelkey_state.h"

struct udev *g_udev = nullptr;
struct udev_monitor *g_mon = nullptr;

static void create_outputs_from_decls() {
  for (auto &out : aelkey_state.output_decls) {
    if (out.id.empty()) {
      continue;
    }
    if (aelkey_state.uinput_devices.count(out.id)) {
      continue;
    }
    libevdev_uinput *uidev = create_output_device(out);
    if (uidev) {
      aelkey_state.uinput_devices[out.id] = uidev;
    }
  }
}

static void attach_inputs_from_decls() {
  for (auto &decl : aelkey_state.input_decls) {
    std::string devnode = match_device(decl);
    if (devnode.empty()) {
      continue;
    }

    if (aelkey_state.devnode_to_fd.find(devnode) != aelkey_state.devnode_to_fd.end()) {
      std::cout << "Device already attached: " << devnode << std::endl;
      continue;
    }

    int newfd = attach_device(
        devnode, decl, aelkey_state.input_map, aelkey_state.frames, aelkey_state.epfd
    );
    if (newfd >= 0) {
      aelkey_state.devnode_to_fd[devnode] = newfd;
    } else {
      std::cerr << "Failed to attach input: " << decl.id << " (" << devnode << ")" << std::endl;
    }
  }
}

static int init_impl(lua_State *L) {
  if (aelkey_state.epfd >= 0) {
    return 0;  // already initialized
  }

  int epfd = epoll_create1(0);
  if (epfd < 0) {
    return luaL_error(L, "epoll_create1 failed");
  }
  aelkey_state.epfd = epfd;

  g_udev = udev_new();
  if (!g_udev) {
    return luaL_error(L, "udev_new failed");
  }

  g_mon = udev_monitor_new_from_netlink(g_udev, "udev");
  if (!g_mon) {
    return luaL_error(L, "udev_monitor_new failed");
  }

  udev_monitor_filter_add_match_subsystem_devtype(g_mon, "input", nullptr);
  udev_monitor_filter_add_match_subsystem_devtype(g_mon, "hidraw", nullptr);
  udev_monitor_enable_receiving(g_mon);

  int mon_fd = udev_monitor_get_fd(g_mon);
  aelkey_state.udev_fd = mon_fd;

  struct epoll_event ev{};
  ev.events = EPOLLIN;
  ev.data.fd = mon_fd;
  if (epoll_ctl(aelkey_state.epfd, EPOLL_CTL_ADD, mon_fd, &ev) < 0) {
    return luaL_error(L, "epoll_ctl add udev failed");
  }

  return 0;
}

static void handle_udev_remove(lua_State *L, const std::string &devnode) {
  if (devnode.empty()) {
    return;
  }

  auto it = aelkey_state.devnode_to_fd.find(devnode);
  if (it == aelkey_state.devnode_to_fd.end()) {
    return;
  }

  int dfd = it->second;
  epoll_ctl(aelkey_state.epfd, EPOLL_CTL_DEL, dfd, nullptr);

  auto im = aelkey_state.input_map.find(dfd);
  if (im != aelkey_state.input_map.end()) {
    auto &ctx = im->second;

    if (!ctx.decl.callback_state.empty()) {
      lua_getglobal(L, ctx.decl.callback_state.c_str());
      if (lua_isfunction(L, -1)) {
        lua_pushstring(L, "disconnect");
        if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
          std::cerr << "Lua state_callback error: " << lua_tostring(L, -1) << std::endl;
          lua_pop(L, 1);
        }
      } else {
        lua_pop(L, 1);
      }
    }

    if (ctx.idev) {
      libevdev_grab(ctx.idev, LIBEVDEV_UNGRAB);
      libevdev_free(ctx.idev);
    }
    aelkey_state.input_map.erase(im);
    aelkey_state.frames.erase(dfd);
  }

  close(dfd);
  aelkey_state.devnode_to_fd.erase(it);
}

static void handle_udev_add(lua_State *L, const std::string &devnode) {
  if (devnode.empty()) {
    return;
  }

  if (aelkey_state.devnode_to_fd.find(devnode) != aelkey_state.devnode_to_fd.end()) {
    std::cout << "Device already attached: " << devnode << std::endl;
    return;
  }

  for (auto &decl : aelkey_state.input_decls) {
    std::string candidate = match_device(decl);
    if (candidate == devnode) {
      int newfd = attach_device(
          devnode, decl, aelkey_state.input_map, aelkey_state.frames, aelkey_state.epfd
      );
      if (newfd >= 0) {
        aelkey_state.devnode_to_fd[devnode] = newfd;

        if (!decl.callback_state.empty()) {
          lua_getglobal(L, decl.callback_state.c_str());
          if (lua_isfunction(L, -1)) {
            lua_pushstring(L, "reconnect");
            if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
              std::cerr << "Lua state_callback error: " << lua_tostring(L, -1) << std::endl;
              lua_pop(L, 1);
            }
          } else {
            lua_pop(L, 1);
          }
        }
      } else {
        std::cerr << "Failed to reattach input: " << decl.id << " (" << devnode << ")"
                  << std::endl;
      }
      break;
    }
  }
}

static void dispatch_tick(lua_State *L, int fd_ready) {
  auto it_tick = aelkey_state.tick_callbacks.find(fd_ready);
  if (it_tick == aelkey_state.tick_callbacks.end()) {
    return;
  }

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
        lua_pop(L, 1);
      }
    }
  } else if (r < 0 && errno != EAGAIN) {
    perror("read(timerfd)");
  }
}

static void cleanup_fd_on_hup_err(lua_State *L, int fd_ready) {
  auto it_fd = aelkey_state.devnode_to_fd.begin();
  for (; it_fd != aelkey_state.devnode_to_fd.end(); ++it_fd) {
    if (it_fd->second == fd_ready) {
      break;
    }
  }

  epoll_ctl(aelkey_state.epfd, EPOLL_CTL_DEL, fd_ready, nullptr);

  auto input_it = aelkey_state.input_map.find(fd_ready);
  if (input_it != aelkey_state.input_map.end()) {
    auto &ctx = input_it->second;

    if (!ctx.decl.callback_state.empty()) {
      lua_getglobal(L, ctx.decl.callback_state.c_str());
      if (lua_isfunction(L, -1)) {
        lua_pushstring(L, "disconnect");
        if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
          std::cerr << "Lua state_callback error: " << lua_tostring(L, -1) << std::endl;
          lua_pop(L, 1);
        }
      } else {
        lua_pop(L, 1);
      }
    }

    if (ctx.idev) {
      libevdev_grab(ctx.idev, LIBEVDEV_UNGRAB);
      libevdev_free(ctx.idev);
    }
    aelkey_state.input_map.erase(input_it);
  }

  aelkey_state.frames.erase(fd_ready);
  close(fd_ready);
  if (it_fd != aelkey_state.devnode_to_fd.end()) {
    aelkey_state.devnode_to_fd.erase(it_fd);
  }
}

static void dispatch_hidraw(lua_State *L, int fd_ready, InputCtx &ctx) {
  uint8_t buf[64];
  ssize_t r = read(fd_ready, buf, sizeof(buf));
  if (r > 0 && !ctx.decl.callback_events.empty()) {
    lua_getglobal(L, ctx.decl.callback_events.c_str());
    if (lua_isfunction(L, -1)) {
      lua_newtable(L);

      lua_pushstring(L, "device");
      lua_pushstring(L, ctx.decl.id.c_str());
      lua_settable(L, -3);

      lua_pushstring(L, "report");
      lua_pushlstring(L, (const char *)buf, r);
      lua_settable(L, -3);

      if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
        std::cerr << "Lua hidraw callback error: " << lua_tostring(L, -1) << std::endl;
        lua_pop(L, 1);
      }
    } else {
      lua_pop(L, 1);
    }
  }
}

static void dispatch_evdev(lua_State *L, int fd_ready, InputCtx &ctx) {
  if (!ctx.idev) {
    return;
  }

  auto fit = aelkey_state.frames.find(fd_ready);
  if (fit == aelkey_state.frames.end()) {
    return;
  }
  auto &frame = fit->second;

  struct input_event ev;
  while (true) {
    int rc = libevdev_next_event(ctx.idev, LIBEVDEV_READ_FLAG_NORMAL, &ev);
    if (rc == 0) {
      // Accumulate into the frame
      frame.push_back(ev);

      // On SYN_REPORT, flush frame to callback_events
      if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
        if (!ctx.decl.callback_events.empty()) {
          lua_getglobal(L, ctx.decl.callback_events.c_str());
          if (lua_isfunction(L, -1)) {
            // Build events array
            lua_newtable(L);
            int idx = 1;
            for (const auto &e : frame) {
              lua_newtable(L);

              lua_pushstring(L, "device");
              lua_pushstring(L, ctx.decl.id.c_str());
              lua_settable(L, -3);

              const char *tname = libevdev_event_type_get_name(e.type);
              const char *cname = libevdev_event_code_get_name(e.type, e.code);

              lua_pushstring(L, "type_name");
              lua_pushstring(L, tname ? tname : "");
              lua_settable(L, -3);

              lua_pushstring(L, "code_name");
              lua_pushstring(L, cname ? cname : "");
              lua_settable(L, -3);

              lua_pushstring(L, "type");
              lua_pushinteger(L, e.type);
              lua_settable(L, -3);

              lua_pushstring(L, "code");
              lua_pushinteger(L, e.code);
              lua_settable(L, -3);

              lua_pushstring(L, "value");
              lua_pushinteger(L, e.value);
              lua_settable(L, -3);

              lua_pushstring(L, "sec");
              lua_pushinteger(L, e.time.tv_sec);
              lua_settable(L, -3);

              lua_pushstring(L, "usec");
              lua_pushinteger(L, e.time.tv_usec);
              lua_settable(L, -3);

              lua_rawseti(L, -2, idx++);
            }

            if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
              std::cerr << "Lua event callback error: " << lua_tostring(L, -1) << std::endl;
              lua_pop(L, 1);
            }
          } else {
            lua_pop(L, 1);
          }
        }
        frame.clear();
      }
    } else if (rc == -EAGAIN) {
      break;  // no more events
    } else if (rc == LIBEVDEV_READ_STATUS_SYNC) {
      // You can implement resync as needed
      break;
    } else {
      // Error; caller will handle HUP/ERR
      break;
    }
  }
}

int lua_stop(lua_State *L) {
  aelkey_state.should_stop = true;
  return 0;
}

void handle_signal(int sig) {
  aelkey_state.should_stop = true;
}

int lua_start(lua_State *L) {
  // signal handlers
  std::signal(SIGHUP, handle_signal);   // terminal hangup
  std::signal(SIGINT, handle_signal);   // interactive interrupt (Ctrl+C)
  std::signal(SIGTERM, handle_signal);  // termination request (kill, systemd stop)

  // 1) Ensure init is done (epoll + udev monitor)
  if (aelkey_state.epfd < 0) {
    int rc = init_impl(L);
    if (rc != 0) {
      // lua_init already pushed an error or returned an error code
      return rc;
    }
  }

  // 2) Parse declarations from Lua and perform initial setup
  // These helpers should read from the script's tables and fill:
  //   aelkey_state.output_decls and aelkey_state.input_decls
  parse_outputs_from_lua(L);
  parse_inputs_from_lua(L);

  create_outputs_from_decls();
  attach_inputs_from_decls();

  // 3) Blocking epoll loop
  constexpr int MAX_EVENTS = 64;
  struct epoll_event events[MAX_EVENTS];
  aelkey_state.should_stop = false;

  while (!aelkey_state.should_stop) {
    int n = epoll_wait(aelkey_state.epfd, events, MAX_EVENTS, -1);  // block until event
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      perror("epoll_wait");
      break;
    }

    for (int i = 0; i < n; ++i) {
      int fd_ready = events[i].data.fd;
      uint32_t evmask = events[i].events;

      // udev hotplug
      if (fd_ready == aelkey_state.udev_fd) {
        struct udev_device *dev = udev_monitor_receive_device(g_mon);
        if (!dev) {
          continue;
        }

        const char *action = udev_device_get_action(dev);
        const char *node = udev_device_get_devnode(dev);
        std::string devnode = node ? node : "";

        if (action && strcmp(action, "add") == 0) {
          handle_udev_add(L, devnode);
        } else if (action && strcmp(action, "remove") == 0) {
          handle_udev_remove(L, devnode);
        }
        udev_device_unref(dev);
        continue;
      }

      // timerfd ticks
      if (aelkey_state.tick_callbacks.count(fd_ready)) {
        dispatch_tick(L, fd_ready);
        continue;
      }

      // Input fds or cleanup
      if ((evmask & (EPOLLHUP | EPOLLERR)) != 0) {
        cleanup_fd_on_hup_err(L, fd_ready);
        continue;
      }
      if ((evmask & EPOLLIN) == 0) {
        continue;
      }

      auto it = aelkey_state.input_map.find(fd_ready);
      if (it == aelkey_state.input_map.end()) {
        continue;
      }

      InputCtx &ctx = it->second;
      if (ctx.decl.type == "hidraw") {
        dispatch_hidraw(L, fd_ready, ctx);
      } else {
        dispatch_evdev(L, fd_ready, ctx);
      }
    }
  }

  // 4) Cleanup all resources
  for (auto &kv : aelkey_state.input_map) {
    int fd = kv.first;
    if (kv.second.idev) {
      libevdev_grab(kv.second.idev, LIBEVDEV_UNGRAB);
      libevdev_free(kv.second.idev);
    }
    close(fd);
  }
  aelkey_state.input_map.clear();
  aelkey_state.frames.clear();
  aelkey_state.devnode_to_fd.clear();

  for (auto &kv : aelkey_state.uinput_devices) {
    libevdev_uinput_destroy(kv.second);
  }
  aelkey_state.uinput_devices.clear();

  if (aelkey_state.udev_fd >= 0) {
    epoll_ctl(aelkey_state.epfd, EPOLL_CTL_DEL, aelkey_state.udev_fd, nullptr);
    aelkey_state.udev_fd = -1;
  }
  if (g_mon) {
    udev_monitor_unref(g_mon);
    g_mon = nullptr;
  }
  if (g_udev) {
    udev_unref(g_udev);
    g_udev = nullptr;
  }
  if (aelkey_state.epfd >= 0) {
    close(aelkey_state.epfd);
    aelkey_state.epfd = -1;
  }

  lua_pushboolean(L, 1);
  return 1;
}

#include "aelkey_core.h"

#include <csignal>
#include <cstring>
#include <iostream>
#include <string>

#include <libevdev/libevdev-uinput.h>
#include <libudev.h>
#include <libusb-1.0/libusb.h>
#include <lua.hpp>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include "aelkey_device.h"
#include "aelkey_state.h"
#include "device_udev.h"
#include "luacompat.h"

static InputDecl detach_input_device(const std::string &dev_id) {
  InputDecl decl{};

  auto im = aelkey_state.input_map.find(dev_id);
  if (im == aelkey_state.input_map.end()) {
    return decl;  // nothing to detach
  }

  InputCtx &ctx = im->second;
  decl = ctx.decl;

  // Remove from epoll if fd is valid
  if (aelkey_state.epfd >= 0 && ctx.fd >= 0) {
    epoll_ctl(aelkey_state.epfd, EPOLL_CTL_DEL, ctx.fd, nullptr);
  }

  // Free libevdev resources if present
  if (ctx.idev) {
    libevdev_grab(ctx.idev, LIBEVDEV_UNGRAB);
    libevdev_free(ctx.idev);
    ctx.idev = nullptr;
  }

  // Close fd if valid
  if (ctx.fd >= 0) {
    close(ctx.fd);
    ctx.fd = -1;
  }

  // Close libusb handle if present
  if (ctx.usb_handle) {
    libusb_close(ctx.usb_handle);
    ctx.usb_handle = nullptr;
  }

  // Erase from maps keyed by string id
  aelkey_state.input_map.erase(im);
  aelkey_state.frames.erase(dev_id);

  return decl;
}

static void handle_udev_remove(lua_State *L, const std::string &devnode) {
  if (devnode.empty()) {
    return;
  }

  for (auto &decl : aelkey_state.input_decls) {
    std::string candidate = match_device(decl);
    if (candidate == devnode) {
      InputDecl removed = detach_input_device(decl.id);
      if (!removed.id.empty()) {
        notify_state_change(L, removed, "disconnect");
      }
      break;
    }
  }
}

static void handle_udev_add(lua_State *L, const std::string &devnode) {
  if (devnode.empty()) {
    return;
  }

  // already attached?
  for (auto &decl : aelkey_state.input_decls) {
    std::string candidate = match_device(decl);
    if (candidate == devnode) {
      if (aelkey_state.input_map.find(decl.id) != aelkey_state.input_map.end()) {
        std::cout << "Device already attached: " << decl.id << std::endl;
        return;
      }

      // try to attach
      if (attach_input_device(devnode, decl)) {
        notify_state_change(L, decl, "connect");
      }
      break;
    }
  }
}

static void dispatch_hidraw(lua_State *L, int fd_ready, InputCtx &ctx) {
  uint8_t buf[4096];
  ssize_t r = read(fd_ready, buf, sizeof(buf));

  if (!ctx.decl.callback_events.empty()) {
    lua_getglobal(L, ctx.decl.callback_events.c_str());
    if (lua_isfunction(L, -1)) {
      lua_newtable(L);

      lua_pushstring(L, ctx.decl.id.c_str());
      lua_setfield(L, -2, "device");

      if (r > 0) {
        lua_pushlstring(L, (const char *)buf, r);
        lua_setfield(L, -2, "data");

        lua_pushinteger(L, r);
        lua_setfield(L, -2, "size");

        lua_pushstring(L, "ok");
        lua_setfield(L, -2, "status");
      } else if (r == 0) {
        // EOF / disconnect
        lua_pushstring(L, "disconnect");
        lua_setfield(L, -2, "status");
      } else {
        // error
        lua_pushstring(L, strerror(errno));
        lua_setfield(L, -2, "status");
      }

      if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
        std::cerr << "Lua hidraw callback error: " << lua_tostring(L, -1) << std::endl;
        lua_pop(L, 1);
      }
    } else {
      lua_pop(L, 1);
    }
  }
}

static void dispatch_evdev(lua_State *L, InputCtx &ctx) {
  if (!ctx.idev) {
    return;
  }

  auto fit = aelkey_state.frames.find(ctx.decl.id);
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

              lua_pushstring(L, ctx.decl.id.c_str());
              lua_setfield(L, -2, "device");

              const char *tname = libevdev_event_type_get_name(e.type);
              const char *cname = libevdev_event_code_get_name(e.type, e.code);

              lua_pushstring(L, tname ? tname : "");
              lua_setfield(L, -2, "type_name");

              lua_pushstring(L, cname ? cname : "");
              lua_setfield(L, -2, "code_name");

              lua_pushinteger(L, e.type);
              lua_setfield(L, -2, "type");

              lua_pushinteger(L, e.code);
              lua_setfield(L, -2, "code");

              lua_pushinteger(L, e.value);
              lua_setfield(L, -2, "value");

              lua_pushinteger(L, e.time.tv_sec);
              lua_setfield(L, -2, "sec");

              lua_pushinteger(L, e.time.tv_usec);
              lua_setfield(L, -2, "usec");

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
  aelkey_state.loop_should_stop = true;
  return 0;
}

void handle_signal(int sig) {
  aelkey_state.loop_should_stop = true;
}

int lua_start(lua_State *L) {
  if (aelkey_state.active_mode == AelkeyState::ActiveMode::DAEMON) {
    luaL_error(L, "cannot start event loop while daemon is running");
    return 1;
  } else if (aelkey_state.active_mode == AelkeyState::ActiveMode::LOOP) {
    lua_warning(L, "event loop is already running");
    return 1;
  }

  aelkey_state.aelkey_set_mode(AelkeyState::ActiveMode::LOOP);

  // signal handlers
  std::signal(SIGHUP, handle_signal);   // terminal hangup
  std::signal(SIGINT, handle_signal);   // interactive interrupt (Ctrl+C)
  std::signal(SIGTERM, handle_signal);  // termination request (kill, systemd stop)

  // open inputs and outputs tables
  lua_open_device(L);

  // Blocking epoll loop
  constexpr int MAX_EVENTS = 64;
  struct epoll_event events[MAX_EVENTS];

  while (!aelkey_state.loop_should_stop) {
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

      // libusb poll fds
      if (aelkey_state.libusb_fd_set.count(fd_ready)) {
        timeval tv{ 0, 0 };
        int rc = libusb_handle_events_timeout_completed(aelkey_state.g_libusb, &tv, nullptr);
        if (rc != 0) {
          std::cerr << "libusb_handle_events error: " << libusb_error_name(rc) << std::endl;
        }
        continue;  // handled; go to next epoll event
      }

      // udev hotplug
      if (fd_ready == aelkey_state.udev_fd) {
        struct udev_device *dev = udev_monitor_receive_device(aelkey_state.g_mon);
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
      if (aelkey_state.scheduler && aelkey_state.scheduler->owns_fd(fd_ready)) {
        aelkey_state.scheduler->handle_event(fd_ready);
        continue;
      }

      // look up InputCtx
      InputCtx *ctx_ptr = nullptr;
      for (auto &kv : aelkey_state.input_map) {
        if (kv.second.fd == fd_ready) {
          ctx_ptr = &kv.second;
          break;
        }
      }
      if (!ctx_ptr) {
        continue;
      }

      // Input fds or cleanup
      if ((evmask & (EPOLLHUP | EPOLLERR)) != 0) {
        InputDecl decl = detach_input_device(ctx_ptr->decl.id);
        if (!decl.id.empty()) {
          notify_state_change(L, decl, "disconnect");
        }
        continue;
      }
      if ((evmask & EPOLLIN) == 0) {
        continue;
      }

      if (ctx_ptr->decl.type == "hidraw") {
        dispatch_hidraw(L, fd_ready, *ctx_ptr);
      } else if (ctx_ptr->decl.type == "evdev") {
        dispatch_evdev(L, *ctx_ptr);
      }
    }
  }

  // Cleanup all resources
  // Detach all devices
  for (auto &kv : aelkey_state.input_map) {
    InputCtx &ctx = kv.second;

    // Evdev/hidraw cleanup
    if (ctx.idev) {
      libevdev_grab(ctx.idev, LIBEVDEV_UNGRAB);
      libevdev_free(ctx.idev);
    }
    if (ctx.fd >= 0) {
      close(ctx.fd);
      ctx.fd = -1;
    }

    // Libusb cleanup
    if (ctx.usb_handle) {
      libusb_close(ctx.usb_handle);
      ctx.usb_handle = nullptr;
    }
  }
  aelkey_state.input_map.clear();
  aelkey_state.frames.clear();

  // Destroy uinput devices
  for (auto &kv : aelkey_state.uinput_devices) {
    libevdev_uinput_destroy(kv.second);
  }
  aelkey_state.uinput_devices.clear();

  // Tear down global monitoring state
  if (aelkey_state.udev_fd >= 0) {
    epoll_ctl(aelkey_state.epfd, EPOLL_CTL_DEL, aelkey_state.udev_fd, nullptr);
    close(aelkey_state.udev_fd);
    aelkey_state.udev_fd = -1;
  }
  if (aelkey_state.g_mon) {
    udev_monitor_unref(aelkey_state.g_mon);
    aelkey_state.g_mon = nullptr;
  }
  if (aelkey_state.g_udev) {
    udev_unref(aelkey_state.g_udev);
    aelkey_state.g_udev = nullptr;
  }
  if (aelkey_state.scheduler) {
    delete aelkey_state.scheduler;
    aelkey_state.scheduler = nullptr;
  }
  if (aelkey_state.epfd >= 0) {
    close(aelkey_state.epfd);
    aelkey_state.epfd = -1;
  }
  if (aelkey_state.g_libusb) {
    libusb_exit(aelkey_state.g_libusb);
    aelkey_state.g_libusb = nullptr;
  }

  // Reset mode
  aelkey_state.aelkey_set_mode(AelkeyState::ActiveMode::NONE);

  lua_pushboolean(L, 1);
  return 1;
}

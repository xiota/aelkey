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

#include "aelkey_state.h"
#include "device_udev.h"
#include "luacompat.h"

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

static void attach_inputs_from_decls(lua_State *L) {
  for (auto &decl : aelkey_state.input_decls) {
    std::string devnode = match_device(decl);
    if (devnode.empty()) {
      continue;
    }
    if (attach_input_device(devnode, decl)) {
      notify_state_change(L, decl, "connect");
    }
  }
}

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

static void dispatch_tick(lua_State *L, int timer_fd) {
  auto it_tick = aelkey_state.tick_callbacks.find(timer_fd);
  if (it_tick == aelkey_state.tick_callbacks.end()) {
    return;
  }

  uint64_t expirations = 0;
  ssize_t r = read(timer_fd, &expirations, sizeof(expirations));
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

// Map libusb_transfer_type enum → string
static const char *transfer_type_to_string(uint8_t type) {
  switch (type) {
    case LIBUSB_TRANSFER_TYPE_CONTROL:
      return "control";
    case LIBUSB_TRANSFER_TYPE_ISOCHRONOUS:
      return "iso";
    case LIBUSB_TRANSFER_TYPE_BULK:
      return "bulk";
    case LIBUSB_TRANSFER_TYPE_INTERRUPT:
      return "interrupt";
    default:
      return "unknown";
  }
}

// Map libusb_transfer_status enum → string
static const char *transfer_status_to_string(libusb_transfer_status status) {
  switch (status) {
    case LIBUSB_TRANSFER_COMPLETED:
      return "ok";
    case LIBUSB_TRANSFER_ERROR:
      return "error";
    case LIBUSB_TRANSFER_TIMED_OUT:
      return "timeout";
    case LIBUSB_TRANSFER_CANCELLED:
      return "cancelled";
    case LIBUSB_TRANSFER_STALL:
      return "stall";
    case LIBUSB_TRANSFER_NO_DEVICE:
      return "no_device";
    case LIBUSB_TRANSFER_OVERFLOW:
      return "overflow";
    default:
      return "unknown";
  }
}

static void LIBUSB_CALL dispatch_libusb(libusb_transfer *transfer) {
  auto *ud = static_cast<std::pair<InputCtx *, lua_State *> *>(transfer->user_data);
  InputCtx *ctx = ud->first;
  lua_State *L = ud->second;

  if (!ctx->decl.callback_events.empty()) {
    lua_getglobal(L, ctx->decl.callback_events.c_str());
    if (lua_isfunction(L, -1)) {
      lua_newtable(L);

      lua_pushstring(L, "device");
      lua_pushstring(L, ctx->decl.id.c_str());
      lua_settable(L, -3);

      lua_pushstring(L, "data");
      lua_pushlstring(
          L, reinterpret_cast<const char *>(transfer->buffer), transfer->actual_length
      );
      lua_settable(L, -3);

      lua_pushstring(L, "endpoint");
      lua_pushinteger(L, transfer->endpoint);
      lua_settable(L, -3);

      lua_pushstring(L, "transfer");
      lua_pushstring(L, transfer_type_to_string(transfer->type));
      lua_settable(L, -3);

      lua_pushstring(L, "status");
      lua_pushstring(L, transfer_status_to_string(transfer->status));
      lua_settable(L, -3);

      if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
        std::cerr << "Lua libusb callback error: " << lua_tostring(L, -1) << std::endl;
        lua_pop(L, 1);
      }
    } else {
      lua_pop(L, 1);
    }
  }

  // resubmit for continuous streaming
  int rc = libusb_submit_transfer(transfer);
  if (rc != 0) {
    std::cerr << "libusb_submit_transfer error: " << libusb_error_name(rc) << std::endl;
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

      lua_pushstring(L, "data");
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

  // 1) Ensure init is done (epoll + udev monitor)
  if (aelkey_state.epfd < 0) {
    int rc = device_udev_init(L);
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
  attach_inputs_from_decls(L);

  // 3) Blocking epoll loop
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
      if (aelkey_state.tick_callbacks.count(fd_ready)) {
        dispatch_tick(L, fd_ready);
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
      } else if (ctx_ptr->decl.type == "libusb") {
        // completions will be delivered to the transfer callback
        int rc = libusb_handle_events(aelkey_state.g_libusb);
        if (rc != 0) {
          std::cerr << "libusb_handle_events error: " << libusb_error_name(rc) << std::endl;
        }
      } else {
        dispatch_evdev(L, *ctx_ptr);
      }
    }
  }

  // 4) Cleanup all resources
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

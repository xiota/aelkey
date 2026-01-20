#include "aelkey_core.h"

#include <csignal>
#include <cstring>
#include <string>

#include <libevdev/libevdev-uinput.h>
#include <libudev.h>
#include <libusb-1.0/libusb.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include <sol/sol.hpp>

#include "aelkey_device.h"
#include "aelkey_haptics.h"
#include "aelkey_state.h"
#include "device_gatt.h"
#include "device_input.h"
#include "device_udev.h"

static void dispatch_hidraw(sol::this_state ts, int fd_ready, InputCtx &ctx) {
  sol::state_view lua(ts);

  uint8_t buf[4096];
  ssize_t r = read(fd_ready, buf, sizeof(buf));

  if (ctx.decl.on_event.empty()) {
    return;
  }

  sol::object obj = lua[ctx.decl.on_event];
  if (!obj.is<sol::function>()) {
    return;
  }

  sol::function cb = obj.as<sol::function>();

  sol::table tbl = lua.create_table();
  tbl["device"] = ctx.decl.id;

  if (r > 0) {
    tbl["data"] = std::string(reinterpret_cast<const char *>(buf), static_cast<std::size_t>(r));
    tbl["size"] = static_cast<int>(r);
    tbl["status"] = "ok";
  } else if (r == 0) {
    tbl["status"] = "disconnect";
  } else {
    tbl["status"] = strerror(errno);
  }

  sol::protected_function pf = cb;
  sol::protected_function_result res = pf(tbl);
  if (!res.valid()) {
    sol::error err = res;
    std::fprintf(stderr, "Lua hidraw callback error: %s\n", err.what());
  }
}

static void dispatch_evdev(sol::this_state ts, InputCtx &ctx) {
  sol::state_view lua(ts);

  if (!ctx.idev) {
    return;
  }

  auto &state = AelkeyState::instance();
  auto fit = state.frames.find(ctx.decl.id);
  if (fit == state.frames.end()) {
    return;
  }
  auto &frame = fit->second;

  struct input_event ev;
  while (true) {
    int rc = libevdev_next_event(ctx.idev, LIBEVDEV_READ_FLAG_NORMAL, &ev);
    if (rc == 0) {
      // Accumulate into the frame
      frame.push_back(ev);

      // On SYN_REPORT, flush frame to on_event
      if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
        if (!ctx.decl.on_event.empty()) {
          sol::object obj = lua[ctx.decl.on_event];
          if (obj.is<sol::function>()) {
            sol::function cb = obj.as<sol::function>();

            sol::table events_tbl = lua.create_table();
            int idx = 1;
            for (const auto &e : frame) {
              sol::table evt = lua.create_table();

              evt["device"] = ctx.decl.id;

              const char *tname = libevdev_event_type_get_name(e.type);
              const char *cname = libevdev_event_code_get_name(e.type, e.code);

              evt["type"] = tname ? tname : "";
              evt["code"] = cname ? cname : "";
              evt["value"] = e.value;
              evt["sec"] = static_cast<int>(e.time.tv_sec);
              evt["usec"] = static_cast<int>(e.time.tv_usec);

              events_tbl[idx++] = evt;
            }

            sol::protected_function pf = cb;
            sol::protected_function_result res = pf(events_tbl);
            if (!res.valid()) {
              sol::error err = res;
              std::fprintf(stderr, "Lua event callback error: %s\n", err.what());
            }
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

void dispatch_haptics(sol::this_state ts, HapticsSourceCtx &src) {
  struct input_event ev;

  ssize_t n = ::read(src.fd, &ev, sizeof(ev));
  if (n < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return;
    }
    perror("read haptics");
    return;
  } else if (n == 0) {
    return;
  } else if (n != sizeof(ev)) {
    return;
  }

  if (ev.type == EV_UINPUT) {
    if (ev.code == UI_FF_UPLOAD) {
      haptics_handle_upload(src, ev.value);
    } else if (ev.code == UI_FF_ERASE) {
      haptics_handle_erase(src, ev.value);
    }
  } else if (ev.type == EV_FF) {
    int virt_id = ev.code;
    int magnitude = ev.value;

    if (magnitude > 0) {
      haptics_handle_play(ts, src, virt_id, magnitude);
    } else {
      haptics_handle_stop(ts, src, virt_id);
    }
  }
}

sol::object loop_stop(sol::this_state ts) {
  sol::state_view lua(ts);
  auto &state = AelkeyState::instance();
  state.loop_should_stop = true;
  return sol::make_object(lua, sol::nil);
}

void handle_signal(int sig) {
  auto &state = AelkeyState::instance();
  state.loop_should_stop = true;
  state.sigint = sig;
}

sol::object loop_start(sol::this_state ts) {
  sol::state_view lua(ts);

  // signal handlers
  std::signal(SIGHUP, handle_signal);   // terminal hangup
  std::signal(SIGINT, handle_signal);   // interactive interrupt (Ctrl+C)
  std::signal(SIGTERM, handle_signal);  // termination request (kill, systemd stop)

  // open inputs and outputs tables (open all devices)
  device_open(ts, sol::optional<std::string>{});  // equivalent to old lua_open_device(L)

  // Blocking epoll loop
  constexpr int MAX_EVENTS = 64;
  struct epoll_event events[MAX_EVENTS];

  auto &state = AelkeyState::instance();
  while (!state.loop_should_stop) {
    int n = epoll_wait(state.epfd, events, MAX_EVENTS, -1);  // block until event
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
      if (state.libusb_fd_set.count(fd_ready)) {
        timeval tv{ 0, 0 };
        int rc = libusb_handle_events_timeout_completed(state.g_libusb, &tv, nullptr);
        if (rc != 0) {
          std::fprintf(stderr, "libusb_handle_events error: %s\n", libusb_error_name(rc));
        }
        continue;  // handled; go to next epoll event
      }

      // D-Bus GATT notifications
      if (fd_ready == state.g_dbus_fd) {
        dispatch_gatt(ts);
        continue;
      }

      // udev hotplug
      if (fd_ready == state.udev_fd) {
        struct udev_device *dev = udev_monitor_receive_device(state.g_mon);
        if (!dev) {
          continue;
        }

        const char *action = udev_device_get_action(dev);
        if (action) {
          if (strcmp(action, "add") == 0) {
            handle_udev_add(ts, dev);
          } else if (strcmp(action, "remove") == 0) {
            handle_udev_remove(ts, dev);
          }
        }

        udev_device_unref(dev);
        continue;
      }

      // timerfd ticks
      if (state.scheduler && state.scheduler->owns_fd(fd_ready)) {
        state.scheduler->handle_event(fd_ready);
        continue;
      }

      // Haptics
      HapticsSourceCtx *src = nullptr;
      for (auto &kv : state.haptics_sources) {
        if (kv.second.fd == fd_ready) {
          src = &kv.second;
          break;
        }
      }

      if (src) {
        if (evmask & (EPOLLHUP | EPOLLERR)) {
          continue;
        }

        if (evmask & EPOLLIN) {
          dispatch_haptics(ts, *src);
        }

        continue;
      }

      // look up InputCtx
      InputCtx *ctx_ptr = nullptr;
      for (auto &kv : state.input_map) {
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
          notify_state_change(ts, decl, "remove");
        }
        continue;
      }
      if ((evmask & EPOLLIN) == 0) {
        continue;
      }

      if (ctx_ptr->decl.type == "hidraw") {
        dispatch_hidraw(ts, fd_ready, *ctx_ptr);
      } else if (ctx_ptr->decl.type == "evdev") {
        dispatch_evdev(ts, *ctx_ptr);
      }
    }
  }

  // Cleanup all resources
  // Detach all devices
  for (auto &kv : state.input_map) {
    InputCtx &ctx = kv.second;

    // Evdev/hidraw cleanup
    if (ctx.idev) {
      libevdev_grab(ctx.idev, LIBEVDEV_UNGRAB);
      libevdev_free(ctx.idev);
    }
    if (ctx.fd >= 0) {
      close(ctx.fd);
      ctx.fd = -1;
      ctx.active = false;
    }

    // Libusb cleanup
    if (ctx.usb_handle) {
      libusb_close(ctx.usb_handle);
      ctx.usb_handle = nullptr;
      ctx.active = false;
    }
  }
  state.input_map.clear();
  state.frames.clear();

  // Destroy uinput devices
  for (auto &kv : state.uinput_devices) {
    libevdev_uinput_destroy(kv.second);
  }
  state.uinput_devices.clear();

  // Tear down global monitoring state
  if (state.udev_fd >= 0) {
    epoll_ctl(state.epfd, EPOLL_CTL_DEL, state.udev_fd, nullptr);
    state.udev_fd = -1;
  }
  if (state.g_mon) {
    udev_monitor_unref(state.g_mon);
    state.g_mon = nullptr;
  }
  if (state.g_udev) {
    udev_unref(state.g_udev);
    state.g_udev = nullptr;
  }
  if (state.scheduler) {
    delete state.scheduler;
    state.scheduler = nullptr;
  }
  if (state.epfd >= 0) {
    close(state.epfd);
    state.epfd = -1;
  }
  if (state.g_libusb) {
    libusb_exit(state.g_libusb);
    state.g_libusb = nullptr;
  }

  if (state.sigint != 0) {
    std::signal(SIGHUP, SIG_DFL);
    std::signal(SIGINT, SIG_DFL);
    std::signal(SIGTERM, SIG_DFL);

    int sig = state.sigint;
    std::raise(sig);
  }

  return sol::make_object(lua, true);
}

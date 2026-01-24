#include "aelkey_core.h"

#include <csignal>
#include <cstring>
#include <string>
#include <string_view>

#include <libevdev/libevdev-uinput.h>
#include <libudev.h>
#include <libusb-1.0/libusb.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include <sol/sol.hpp>

#include "aelkey_device.h"
#include "aelkey_state.h"
#include "device_input.h"
#include "dispatcher.h"
#include "dispatcher_udev.h"

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

      // If epoll stored a pointer, treat it as a Dispatcher.
      void *ptr = events[i].data.ptr;
      void *fd_cast = reinterpret_cast<void *>(static_cast<uintptr_t>(fd_ready));
      if (ptr != nullptr && ptr != fd_cast) {
        auto *payload = static_cast<EpollPayload *>(ptr);
        payload->dispatcher->handle_event(payload, events[i].events);
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
          DispatcherUdev::instance().notify_state_change(ctx_ptr->decl, "remove");
        }
        continue;
      }
      if ((evmask & EPOLLIN) == 0) {
        continue;
      }

      if (ctx_ptr->decl.type == "evdev") {
        try_evdev_grab(*ctx_ptr);
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
  if (state.epfd >= 0) {
    close(state.epfd);
    state.epfd = -1;
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

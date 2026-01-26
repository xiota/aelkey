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

      // If epoll stored a pointer, treat it as a Dispatcher.
      void *ptr = events[i].data.ptr;
      void *fd_cast = reinterpret_cast<void *>(static_cast<uintptr_t>(fd_ready));
      if (ptr != nullptr && ptr != fd_cast) {
        auto *payload = static_cast<EpollPayload *>(ptr);
        payload->dispatcher->handle_event(payload, events[i].events);
        continue;
      }
    }
  }

  // Cleanup all resources
  // Detach all devices
  for (auto &kv : state.input_map) {
    InputCtx &ctx = kv.second;

    if (ctx.decl.type == "evdev") {
      // DispatcherEvdev already cleaned it up
      continue;
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

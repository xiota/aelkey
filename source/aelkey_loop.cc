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
#include "device_declarations.h"
#include "device_manager.h"
#include "dispatcher.h"
#include "dispatcher_udev.h"
#include "util/scoped_timer.h"

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

    for (int i = 0; i < n; ++i) {
      auto *payload = static_cast<EpollPayload *>(events[i].data.ptr);
      if (payload->dead) {
        continue;
      }
      payload->dispatcher->handle_event(payload, events[i].events);
    }

    // for (auto &[type, dispatcher] : dispatcher_registry()) {
    //   dispatcher->flush_deferred();
    // }
  }

  // Cleanup all resources

  // Detach all devices
  std::vector<std::string> ids;
  ids.reserve(state.input_map.size());

  for (auto &kv : state.input_map) {
    ids.push_back(kv.first);
  }
  for (const auto &id : ids) {
    // mutates aelkey_state.input_map
    DeviceManager::instance().detach(id);
  }

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

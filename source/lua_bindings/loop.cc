#include "lua_bindings/loop.h"

#include <cstring>
#include <string>
#include <string_view>

#include <libudev.h>
#include <libusb-1.0/libusb.h>
#include <sol/sol.hpp>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include "aelkey_state.h"
#include "device_declarations.h"
#include "dispatcher.h"
#include "lua_bindings/core.h"
#include "manager_device.h"
#include "router_watch_list.h"
#include "signal_handler.h"

sol::object loop_stop(sol::this_state ts) {
  sol::state_view lua(ts);
  auto &state = AelkeyState::instance();
  state.loop_should_stop = true;
  return sol::make_object(lua, sol::nil);
}

sol::object loop_start(sol::this_state ts) {
  auto &state = AelkeyState::instance();
  state.loop_running = true;

  sol::state_view lua(ts);

  // check watchlist
  RouterWatchList::instance().enumerate_now();

  // open inputs and outputs tables (open all devices)
  core_open_device(ts, sol::optional<std::string>{});

  // Blocking epoll loop
  constexpr int MAX_EVENTS = 64;
  struct epoll_event events[MAX_EVENTS];

  bool loop_stop_now = false;
  while (!loop_stop_now) {
    int n = epoll_wait(state.epfd, events, MAX_EVENTS, -1);  // block until event

    for (int i = 0; i < n; ++i) {
      auto *payload = static_cast<EpollPayload *>(events[i].data.ptr);
      if (!payload->dead) {
        payload->dispatch(events[i].events);
      }
    }

    state.notify_epoll_cycle();

    if (state.loop_should_stop && state.is_safe_to_stop()) {
      loop_stop_now = true;
    }
  }

  state.loop_running = false;

  loop_cleanup();

  return sol::make_object(lua, true);
}

void loop_cleanup() {
  auto &state = AelkeyState::instance();
  auto &devmgr = ManagerDevice::instance();

  state.notify_shutdown();

  // Detach all devices
  std::vector<std::string> ids;
  ids.reserve(state.input_map.size());

  for (auto &kv : state.input_map) {
    ids.push_back(kv.first);
  }
  for (const auto &id : ids) {
    // mutates aelkey_state.input_map
    devmgr.detach_output(id);
  }

  for (int i = 0; i < 3; ++i) {
    state.notify_epoll_cycle();
  }

  // Tear down global monitoring state
  if (state.epfd >= 0) {
    close(state.epfd);
    state.epfd = -1;
  }
}

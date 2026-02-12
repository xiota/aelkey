#include "lua_bindings/loop.h"

#include <cstring>
#include <string>
#include <string_view>

#include <libudev.h>
#include <libusb-1.0/libusb.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>

#include <sol/sol.hpp>

#include "aelkey_state.h"
#include "device_declarations.h"
#include "device_in_manager.h"
#include "dispatcher.h"
#include "lua_bindings/core.h"
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

  // open inputs and outputs tables (open all devices)
  core_open_device(ts, sol::optional<std::string>{});

  // Blocking epoll loop
  constexpr int MAX_EVENTS = 64;
  struct epoll_event events[MAX_EVENTS];

  while (!state.loop_should_stop) {
    int n = epoll_wait(state.epfd, events, MAX_EVENTS, -1);  // block until event

    for (int i = 0; i < n; ++i) {
      auto *payload = static_cast<EpollPayload *>(events[i].data.ptr);
      if (payload->dead) {
        continue;
      }
      payload->dispatcher->handle_event(payload, events[i].events);
    }

    for (auto &[type, dispatcher] : dispatcher_registry()) {
      dispatcher->flush_deferred();
    }
  }

  loop_cleanup();

  state.loop_running = false;

  return sol::make_object(lua, true);
}

void loop_cleanup() {
  auto &state = AelkeyState::instance();

  // Detach all devices
  std::vector<std::string> ids;
  ids.reserve(state.input_map.size());

  for (auto &kv : state.input_map) {
    ids.push_back(kv.first);
  }
  for (const auto &id : ids) {
    // mutates aelkey_state.input_map
    DeviceInManager::instance().detach(id);
  }

  // Tear down global monitoring state
  if (state.epfd >= 0) {
    close(state.epfd);
    state.epfd = -1;
  }

  SignalHandler::reraise();
}

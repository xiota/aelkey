#include "lua_bindings.h"

#include <iostream>
#include <string>
#include <unordered_map>

#include <libevdev/libevdev-uinput.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include "globals.h"

// globals
int tick_payload_ref = LUA_NOREF;

static void epoll_remove_fd(int fd) {
  if (fd >= 0) {
    if (epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr) < 0) {
      perror("epoll_ctl EPOLL_CTL_DEL (tick)");
    }
  }
}

int lua_emit(lua_State *L) {
  luaL_checktype(L, 1, LUA_TTABLE);

  const char *dev_id = nullptr;
  int type = 0, code = 0, value = 0;

  lua_getfield(L, 1, "device");
  if (lua_isstring(L, -1)) {
    dev_id = lua_tostring(L, -1);
  }
  lua_pop(L, 1);

  // TYPE
  lua_getfield(L, 1, "type");
  if (lua_isnumber(L, -1)) {
    type = lua_tointeger(L, -1);
  } else if (lua_isstring(L, -1)) {
    const char *tname = lua_tostring(L, -1);
    type = libevdev_event_type_from_name(tname);
  }
  lua_pop(L, 1);

  // CODE
  lua_getfield(L, 1, "code");
  if (lua_isnumber(L, -1)) {
    code = lua_tointeger(L, -1);
  } else if (lua_isstring(L, -1)) {
    const char *cname = lua_tostring(L, -1);
    code = libevdev_event_code_from_name(type, cname);
  }
  lua_pop(L, 1);

  lua_getfield(L, 1, "value");
  value = luaL_checkinteger(L, -1);
  lua_pop(L, 1);

  if (!dev_id) {
    if (uinput_devices.size() == 1) {
      auto it = uinput_devices.begin();
      libevdev_uinput_write_event(it->second, type, code, value);
    } else {
      return luaL_error(L, "emit requires 'device' when multiple output devices are present");
    }
  } else {
    auto it = uinput_devices.find(dev_id);
    if (it == uinput_devices.end()) {
      return luaL_error(L, "Unknown device id: %s", dev_id);
    }
    libevdev_uinput_write_event(it->second, type, code, value);
  }
  return 0;
}

int lua_syn_report(lua_State *L) {
  const char *dev_id = luaL_optstring(L, 1, nullptr);
  if (dev_id) {
    auto it = uinput_devices.find(dev_id);
    if (it != uinput_devices.end()) {
      libevdev_uinput_write_event(it->second, EV_SYN, SYN_REPORT, 0);
    }
  } else {
    for (auto &kv : uinput_devices) {
      libevdev_uinput_write_event(kv.second, EV_SYN, SYN_REPORT, 0);
    }
  }
  return 0;
}

int lua_tick(lua_State *L) {
  int ms = luaL_checkinteger(L, 1);

  if (tfd != -1) {
    close(tfd);
  }

  // disable
  if (ms == 0) {
    if (tfd != -1) {
      epoll_remove_fd(tfd);
      close(tfd);
      tfd = -1;
    }
    if (tick_payload_ref != LUA_NOREF) {
      luaL_unref(L, LUA_REGISTRYINDEX, tick_payload_ref);
      tick_payload_ref = LUA_NOREF;
    }
    return 0;
  }

  // Optional payload (any Lua value). Store by registry ref.
  if (tick_payload_ref != LUA_NOREF) {
    luaL_unref(L, LUA_REGISTRYINDEX, tick_payload_ref);
    tick_payload_ref = LUA_NOREF;
  }
  if (lua_gettop(L) >= 2) {
    lua_pushvalue(L, 2);  // copy payload
    tick_payload_ref = luaL_ref(L, LUA_REGISTRYINDEX);
  }

  // Recreate timerfd cleanly
  if (tfd != -1) {
    epoll_remove_fd(tfd);
    close(tfd);
    tfd = -1;
  }

  tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
  if (tfd < 0) {
    perror("timerfd_create");
    return 0;
  }

  struct itimerspec spec{};
  spec.it_interval.tv_sec = ms / 1000;
  spec.it_interval.tv_nsec = (ms % 1000) * 1000000;
  spec.it_value = spec.it_interval;

  if (timerfd_settime(tfd, 0, &spec, nullptr) < 0) {
    perror("timerfd_settime");
    close(tfd);
    tfd = -1;
    return 0;
  }

  struct epoll_event ev{};
  ev.events = EPOLLIN;
  ev.data.fd = tfd;
  if (epoll_ctl(epfd, EPOLL_CTL_ADD, tfd, &ev) < 0) {
    perror("epoll_ctl EPOLL_CTL_ADD (tick)");
    close(tfd);
    tfd = -1;
    return 0;
  }

  return 0;
}

void register_lua_bindings(lua_State *L) {
  lua_register(L, "emit", lua_emit);
  lua_register(L, "syn_report", lua_syn_report);
  lua_register(L, "tick", lua_tick);

  const char *dump_events_code = R"(
function dump_events(events)
  for _, ev in ipairs(events) do
    print(string.format("device=%s type=%s code=%s value=%d",
                        tostring(ev.device),
                        tostring(ev.type_name),
                        tostring(ev.code_name),
                        tonumber(ev.value or -1)))
  end
end
)";

  luaL_dostring(L, dump_events_code);
}

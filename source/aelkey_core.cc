#include "aelkey_core.h"

#include <ctime>

#include <libevdev/libevdev-uinput.h>
#include <lua.hpp>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include "aelkey_state.h"
#include "tick_scheduler.h"

static void epoll_remove_fd(int fd) {
  if (fd >= 0) {
    if (epoll_ctl(aelkey_state.epfd, EPOLL_CTL_DEL, fd, nullptr) < 0) {
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
    if (aelkey_state.uinput_devices.size() == 1) {
      auto it = aelkey_state.uinput_devices.begin();
      libevdev_uinput_write_event(it->second, type, code, value);
    } else {
      return luaL_error(L, "emit requires 'device' when multiple output devices are present");
    }
  } else {
    auto it = aelkey_state.uinput_devices.find(dev_id);
    if (it == aelkey_state.uinput_devices.end()) {
      return luaL_error(L, "Unknown device id: %s", dev_id);
    }
    libevdev_uinput_write_event(it->second, type, code, value);
  }
  return 0;
}

int lua_syn_report(lua_State *L) {
  const char *dev_id = luaL_optstring(L, 1, nullptr);
  if (dev_id) {
    auto it = aelkey_state.uinput_devices.find(dev_id);
    if (it != aelkey_state.uinput_devices.end()) {
      libevdev_uinput_write_event(it->second, EV_SYN, SYN_REPORT, 0);
    } else {
      return luaL_error(L, "Unknown device id: %s", dev_id);
    }
  } else {
    for (auto &kv : aelkey_state.uinput_devices) {
      libevdev_uinput_write_event(kv.second, EV_SYN, SYN_REPORT, 0);
    }
  }
  return 0;
}

// Assume aelkey_state.scheduler is a TickScheduler*
int lua_tick(lua_State *L) {
  int ms = luaL_checkinteger(L, 1);

  // Case: tick(0) with no callback → stop all
  if (ms == 0 && lua_gettop(L) < 2) {
    aelkey_state.scheduler->cancel_all();
    return 0;
  }

  // Parse callback key (string name OR function)
  TickCb key{};
  if (lua_isstring(L, 2)) {
    key.name = lua_tostring(L, 2);
    key.is_function = false;
  } else if (lua_isfunction(L, 2)) {
    lua_pushvalue(L, 2);
    key.ref = luaL_ref(L, LUA_REGISTRYINDEX);
    key.is_function = true;
  } else {
    return luaL_error(L, "tick callback must be string or function");
  }

  // Cancel existing timer(s) for this callback key (feature parity)
  aelkey_state.scheduler->cancel_matching(key);

  // If ms == 0, we were just canceling (feature parity)
  if (ms == 0) {
    if (key.is_function && key.ref != LUA_NOREF) {
      luaL_unref(L, LUA_REGISTRYINDEX, key.ref);
    }
    return 0;
  }

  // Schedule new repeating timer
  int fd = aelkey_state.scheduler->schedule(ms, key);
  if (fd < 0) {
    // schedule failed; clean up function ref if any
    if (key.is_function && key.ref != LUA_NOREF) {
      luaL_unref(L, LUA_REGISTRYINDEX, key.ref);
    }
    return 0;
  }

  return 0;  // feature parity: no handle returned
}

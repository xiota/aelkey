#include "lua_bindings.h"

#include <filesystem>
#include <iostream>
#include <string>
#include <unordered_map>

#include <libevdev/libevdev-uinput.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include "globals.h"

// globals
int tick_cb_ref = LUA_NOREF;
std::string tick_cb_name;
bool tick_cb_is_function = false;

TickCallback tick_cb;

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

  // Case: tick(0) with no callback → stop all
  if (ms == 0 && lua_gettop(L) < 2) {
    for (auto &[fd, cb] : tick_callbacks) {
      epoll_remove_fd(fd);
      close(fd);
      if (cb.is_function && cb.ref != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, cb.ref);
      }
    }
    tick_callbacks.clear();
    return 0;
  }

  // Parse callback
  TickCallback cb{};
  if (lua_isstring(L, 2)) {
    cb.name = lua_tostring(L, 2);
    cb.is_function = false;
  } else if (lua_isfunction(L, 2)) {
    lua_pushvalue(L, 2);
    cb.ref = luaL_ref(L, LUA_REGISTRYINDEX);
    cb.is_function = true;
  } else {
    return luaL_error(L, "tick callback must be string or function");
  }

  if (ms == 0) {
    // Stop just this callback
    for (auto it = tick_callbacks.begin(); it != tick_callbacks.end();) {
      auto &existing = it->second;
      bool match = false;
      if (cb.is_function && existing.is_function) {
        // Compare actual function values, not registry reference IDs
        lua_rawgeti(L, LUA_REGISTRYINDEX, existing.ref);
        lua_rawgeti(L, LUA_REGISTRYINDEX, cb.ref);
        match = lua_rawequal(L, -1, -2);
        lua_pop(L, 2);
      } else if (!cb.is_function && !existing.is_function) {
        match = (existing.name == cb.name);
      }
      if (match) {
        int fd = it->first;
        epoll_remove_fd(fd);
        close(fd);
        if (existing.is_function && existing.ref != LUA_NOREF) {
          luaL_unref(L, LUA_REGISTRYINDEX, existing.ref);
        }
        it = tick_callbacks.erase(it);
      } else {
        ++it;
      }
    }
    // Unref the temporary cancel reference to avoid leaking
    if (cb.is_function && cb.ref != LUA_NOREF) {
      luaL_unref(L, LUA_REGISTRYINDEX, cb.ref);
    }
    return 0;
  }

  // ms > 0 → stop any existing timer for this callback, then create new timerfd
  for (auto it = tick_callbacks.begin(); it != tick_callbacks.end();) {
    auto &existing = it->second;
    bool match = false;
    if (cb.is_function && existing.is_function) {
      lua_rawgeti(L, LUA_REGISTRYINDEX, existing.ref);
      lua_rawgeti(L, LUA_REGISTRYINDEX, cb.ref);
      match = lua_rawequal(L, -1, -2);
      lua_pop(L, 2);
    } else if (!cb.is_function && !existing.is_function) {
      match = (existing.name == cb.name);
    }
    if (match) {
      int fd = it->first;
      epoll_remove_fd(fd);
      close(fd);
      if (existing.is_function && existing.ref != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, existing.ref);
      }
      it = tick_callbacks.erase(it);
    } else {
      ++it;
    }
  }

  // if we created a temporary ref for cb, keep it for the new timer
  int fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
  if (fd < 0) {
    perror("timerfd_create");
    return 0;
  }

  struct itimerspec spec{};
  spec.it_interval.tv_sec = ms / 1000;
  spec.it_interval.tv_nsec = (ms % 1000) * 1000000;
  spec.it_value = spec.it_interval;
  if (timerfd_settime(fd, 0, &spec, nullptr) < 0) {
    perror("timerfd_settime");
    close(fd);
    return 0;
  }

  struct epoll_event ev{};
  ev.events = EPOLLIN;
  ev.data.fd = fd;
  if (epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev) < 0) {
    perror("epoll_ctl EPOLL_CTL_ADD (tick)");
    close(fd);
    return 0;
  }

  tick_callbacks[fd] = cb;
  return 0;
}

void set_script_path(lua_State *L, const std::filesystem::path &script) {
  std::filesystem::path abs_script = std::filesystem::absolute(script);
  std::string parent = abs_script.parent_path().string();

  std::string new_entry = parent + "/?.lua;" + parent + "/?/init.lua;";

  lua_getglobal(L, "package");
  lua_getfield(L, -1, "path");
  const char *oldpath = lua_tostring(L, -1);
  std::string path = oldpath ? oldpath : "";

  path = new_entry + path;

  lua_pop(L, 1);
  lua_pushstring(L, path.c_str());
  lua_setfield(L, -2, "path");
  lua_pop(L, 1);
}

void register_lua_bindings(lua_State *L) {
  lua_register(L, "emit", lua_emit);
  lua_register(L, "syn_report", lua_syn_report);
  lua_register(L, "tick", lua_tick);

  const char *custom_code = R"(
package.path = package.path:gsub("^%./%?%.lua;", "")

function dump_events(events)
  for _, ev in ipairs(events) do
    print(string.format(
      "device=%s type=%s code=%s value=%d",
      tostring(ev.device),
      tostring(ev.type_name),
      tostring(ev.code_name),
      tonumber(ev.value or -1)
    ))
  end
end

function dump_raw(ev)
  local data = ev.report
  local len = #data
  io.write(string.format("hidraw report (%d bytes):", len))
  for i = 1, len do
    io.write(string.format(" %02X", string.byte(data, i)))
  end
  io.write("\n")
end
)";

  luaL_dostring(L, custom_code);
}

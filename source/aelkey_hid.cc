#include "aelkey_hid.h"

#include <iostream>
#include <vector>

#include <linux/hidraw.h>
#include <lua.hpp>
#include <sys/ioctl.h>
#include <unistd.h>

#include "aelkey_state.h"
#include "luacompat.h"

static int resolve_fd(lua_State *L, int idx) {
  if (lua_type(L, idx) == LUA_TSTRING) {
    const char *id = lua_tostring(L, idx);
    auto it = aelkey_state.devnode_to_fd.find(id);
    if (it == aelkey_state.devnode_to_fd.end()) {
      return -1;  // not found
    }
    return it->second;
  } else {
    return luaL_checkinteger(L, idx);  // still allow raw fd
  }
}

// Retrieve a HID feature report
// get_feature_report(dev_id, report_id)
static int lua_get_feature_report(lua_State *L) {
  int fd = resolve_fd(L, 1);
  if (fd < 0) {
    lua_pushnil(L);
    return 1;
  }
  int report_id = luaL_checkinteger(L, 2);

  const int max_size = 256;
  std::vector<unsigned char> buf(max_size);
  buf[0] = static_cast<unsigned char>(report_id);

  if (ioctl(fd, HIDIOCGFEATURE(max_size), buf.data()) < 0) {
    lua_pushnil(L);
    return 1;
  }

  lua_pushlstring(L, reinterpret_cast<const char *>(buf.data()), max_size);
  return 1;
}

// Retrieve the HID report descriptor
// get_report_descriptor(dev_id)
static int lua_get_report_descriptor(lua_State *L) {
  int fd = resolve_fd(L, 1);
  if (fd < 0) {
    lua_pushnil(L);
    return 1;
  }

  int desc_size;
  if (ioctl(fd, HIDIOCGRDESCSIZE, &desc_size) < 0) {
    lua_pushnil(L);
    return 1;
  }

  struct hidraw_report_descriptor rpt_desc;
  rpt_desc.size = desc_size;

  if (ioctl(fd, HIDIOCGRDESC, &rpt_desc) < 0) {
    lua_pushnil(L);
    return 1;
  }

  lua_pushlstring(L, reinterpret_cast<const char *>(rpt_desc.value), desc_size);
  return 1;
}

// Read an input report from hidraw
// read_input_report(dev_id)
static int lua_read_input_report(lua_State *L) {
  int fd = resolve_fd(L, 1);
  if (fd < 0) {
    lua_pushnil(L);
    return 1;
  }

  const int max_size = 256;
  std::vector<unsigned char> buf(max_size);

  ssize_t n = read(fd, buf.data(), max_size);
  if (n <= 0) {
    lua_pushnil(L);
    return 1;
  }

  lua_pushlstring(L, reinterpret_cast<const char *>(buf.data()), n);
  return 1;
}

// Send a HID feature report
// send_feature_report(dev_id, data)
static int lua_send_feature_report(lua_State *L) {
  int fd = resolve_fd(L, 1);
  if (fd < 0) {
    lua_pushnil(L);
    return 1;
  }
  size_t len;
  const char *data = luaL_checklstring(L, 2, &len);

  if (ioctl(fd, HIDIOCSFEATURE(len), data) < 0) {
    lua_pushboolean(L, 0);
    return 1;
  }

  lua_pushboolean(L, 1);
  return 1;
}

// Write an output report to hidraw
// send_output_report(dev_id, data)
static int lua_send_output_report(lua_State *L) {
  int fd = resolve_fd(L, 1);
  if (fd < 0) {
    lua_pushnil(L);
    return 1;
  }
  size_t len;
  const char *data = luaL_checklstring(L, 2, &len);

  ssize_t n = write(fd, data, len);
  if (n < 0 || static_cast<size_t>(n) != len) {
    lua_pushboolean(L, 0);
    return 1;
  }

  lua_pushboolean(L, 1);
  return 1;
}

extern "C" int luaopen_aelkey_hid(lua_State *L) {
  // clang-format off
  static const luaL_Reg funcs[] = {
    {"get_feature_report", lua_get_feature_report},
    {"get_report_descriptor", lua_get_report_descriptor},
    {"read_input_report",  lua_read_input_report},
    {"send_feature_report", lua_send_feature_report},
    {"send_output_report", lua_send_output_report},
    {nullptr, nullptr}
  };
  // clang-format on

  luaL_newlib(L, funcs);

  return 1;
}

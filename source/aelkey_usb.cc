#include "aelkey_hid.h"

#include <libusb-1.0/libusb.h>
#include <lua.hpp>

#include "aelkey_state.h"
#include "luacompat.h"

static int lua_bulk_transfer(lua_State *L) {
  luaL_checktype(L, 1, LUA_TTABLE);

  // device id
  lua_getfield(L, 1, "device");
  const char *dev_id = luaL_checkstring(L, -1);
  lua_pop(L, 1);

  auto it = aelkey_state.input_map.find(dev_id);
  if (it == aelkey_state.input_map.end() || !it->second.usb_handle) {
    lua_pushnil(L);
    return 1;
  }
  libusb_device_handle *handle = it->second.usb_handle;

  // endpoint
  lua_getfield(L, 1, "endpoint");
  int endpoint = luaL_checkinteger(L, -1);
  lua_pop(L, 1);

  // size
  lua_getfield(L, 1, "size");
  int size = luaL_checkinteger(L, -1);
  lua_pop(L, 1);

  // timeout (optional)
  lua_getfield(L, 1, "timeout");
  int timeout = lua_isnil(L, -1) ? 0 : luaL_checkinteger(L, -1);
  lua_pop(L, 1);

  // Direction: IN if endpoint has LIBUSB_ENDPOINT_IN bit set
  bool is_in = (endpoint & LIBUSB_ENDPOINT_IN) != 0;

  int status = 0;
  int transferred = 0;

  lua_newtable(L);

  if (is_in) {
    // IN transfer: read from device
    std::vector<unsigned char> buf(size);
    status = libusb_bulk_transfer(handle, endpoint, buf.data(), size, &transferred, timeout);

    lua_pushlstring(L, reinterpret_cast<const char *>(buf.data()), transferred);
    lua_setfield(L, -2, "data");
  } else {
    // OUT transfer: send data from Lua
    lua_getfield(L, 1, "data");
    size_t out_len = 0;
    const char *out_data = luaL_optlstring(L, -1, nullptr, &out_len);
    lua_pop(L, 1);

    if (!out_data) {
      out_len = 0;
    }
    if (out_len > static_cast<size_t>(size)) {
      out_len = size;  // clamp to requested size
    }

    status = libusb_bulk_transfer(
        handle,
        endpoint,
        reinterpret_cast<unsigned char *>(const_cast<char *>(out_data)),
        static_cast<int>(out_len),
        &transferred,
        timeout
    );

    // optional: echo back what was sent
    lua_pushlstring(L, out_data, transferred);
    lua_setfield(L, -2, "data");
  }

  // common fields
  lua_pushinteger(L, transferred);
  lua_setfield(L, -2, "size");
  lua_pushinteger(L, status);
  lua_setfield(L, -2, "status");

  return 1;
}

static int lua_control_transfer(lua_State *L) {
  luaL_checktype(L, 1, LUA_TTABLE);

  // device id
  lua_getfield(L, 1, "device");
  const char *dev_id = luaL_checkstring(L, -1);
  lua_pop(L, 1);

  auto it = aelkey_state.input_map.find(dev_id);
  if (it == aelkey_state.input_map.end() || !it->second.usb_handle) {
    lua_pushnil(L);
    return 1;
  }
  libusb_device_handle *handle = it->second.usb_handle;

  // request_type (bmRequestType)
  lua_getfield(L, 1, "request_type");
  int request_type = luaL_checkinteger(L, -1);
  lua_pop(L, 1);

  // request (bRequest)
  lua_getfield(L, 1, "request");
  int request = luaL_checkinteger(L, -1);
  lua_pop(L, 1);

  // value (wValue)
  lua_getfield(L, 1, "value");
  int value = luaL_checkinteger(L, -1);
  lua_pop(L, 1);

  // index (wIndex)
  lua_getfield(L, 1, "index");
  int index = luaL_checkinteger(L, -1);
  lua_pop(L, 1);

  // length (wLength)
  lua_getfield(L, 1, "length");
  int length = luaL_checkinteger(L, -1);
  lua_pop(L, 1);

  // timeout (optional)
  lua_getfield(L, 1, "timeout");
  int timeout = lua_isnil(L, -1) ? 0 : luaL_checkinteger(L, -1);
  lua_pop(L, 1);

  // Direction bit: 0 = OUT, 1 = IN
  bool is_in = (request_type & LIBUSB_ENDPOINT_IN) != 0;

  std::vector<unsigned char> buf(length);
  int status = 0;
  int transferred = 0;

  if (is_in) {
    // IN transfer: read from device
    status = libusb_control_transfer(
        handle,
        static_cast<uint8_t>(request_type),
        static_cast<uint8_t>(request),
        static_cast<uint16_t>(value),
        static_cast<uint16_t>(index),
        buf.data(),
        static_cast<uint16_t>(length),
        static_cast<unsigned int>(timeout)
    );

    transferred = (status >= 0) ? status : 0;
  } else {
    // OUT transfer: send data from Lua
    lua_getfield(L, 1, "data");
    size_t out_len = 0;
    const char *out_data = luaL_optlstring(L, -1, nullptr, &out_len);
    lua_pop(L, 1);

    if (!out_data) {
      out_len = 0;
    }
    if (out_len > static_cast<size_t>(length)) {
      out_len = length;  // clamp to wLength
    }

    status = libusb_control_transfer(
        handle,
        static_cast<uint8_t>(request_type),
        static_cast<uint8_t>(request),
        static_cast<uint16_t>(value),
        static_cast<uint16_t>(index),
        reinterpret_cast<unsigned char *>(const_cast<char *>(out_data)),
        static_cast<uint16_t>(out_len),
        static_cast<unsigned int>(timeout)
    );

    transferred = (status >= 0) ? status : 0;
  }

  // return {data, size, status}
  lua_newtable(L);

  if (is_in) {
    lua_pushlstring(L, reinterpret_cast<const char *>(buf.data()), transferred);
    lua_setfield(L, -2, "data");
  } else {
    // For OUT, echo back the sent data (optional)
    lua_pushlstring(L, reinterpret_cast<const char *>(buf.data()), 0);
    lua_setfield(L, -2, "data");
  }

  lua_pushinteger(L, transferred);
  lua_setfield(L, -2, "size");
  lua_pushinteger(L, status);
  lua_setfield(L, -2, "status");

  return 1;
}

static int lua_interrupt_transfer(lua_State *L) {
  luaL_checktype(L, 1, LUA_TTABLE);

  // device id
  lua_getfield(L, 1, "device");
  const char *dev_id = luaL_checkstring(L, -1);
  lua_pop(L, 1);

  auto it = aelkey_state.input_map.find(dev_id);
  if (it == aelkey_state.input_map.end() || !it->second.usb_handle) {
    lua_pushnil(L);
    return 1;
  }
  libusb_device_handle *handle = it->second.usb_handle;

  // endpoint
  lua_getfield(L, 1, "endpoint");
  int endpoint = luaL_checkinteger(L, -1);
  lua_pop(L, 1);

  // size
  lua_getfield(L, 1, "size");
  int size = luaL_checkinteger(L, -1);
  lua_pop(L, 1);

  // timeout (optional)
  lua_getfield(L, 1, "timeout");
  int timeout = lua_isnil(L, -1) ? 0 : luaL_checkinteger(L, -1);
  lua_pop(L, 1);

  // Direction: IN if endpoint has LIBUSB_ENDPOINT_IN bit set
  bool is_in = (endpoint & LIBUSB_ENDPOINT_IN) != 0;

  int status = 0;
  int transferred = 0;

  lua_newtable(L);

  if (is_in) {
    // IN transfer: read from device
    std::vector<unsigned char> buf(size);
    status =
        libusb_interrupt_transfer(handle, endpoint, buf.data(), size, &transferred, timeout);

    lua_pushlstring(L, reinterpret_cast<const char *>(buf.data()), transferred);
    lua_setfield(L, -2, "data");
  } else {
    // OUT transfer: send data from Lua
    lua_getfield(L, 1, "data");
    size_t out_len = 0;
    const char *out_data = luaL_optlstring(L, -1, nullptr, &out_len);
    lua_pop(L, 1);

    if (!out_data) {
      out_len = 0;
    }
    if (out_len > static_cast<size_t>(size)) {
      out_len = size;  // clamp to requested size
    }

    status = libusb_interrupt_transfer(
        handle,
        endpoint,
        reinterpret_cast<unsigned char *>(const_cast<char *>(out_data)),
        static_cast<int>(out_len),
        &transferred,
        timeout
    );

    // optional: echo back what was sent
    lua_pushlstring(L, out_data, transferred);
    lua_setfield(L, -2, "data");
  }

  // common fields
  lua_pushinteger(L, transferred);
  lua_setfield(L, -2, "size");
  lua_pushinteger(L, status);
  lua_setfield(L, -2, "status");

  return 1;
}

static int lua_submit_transfer(lua_State *L) {
  return 0;
}

extern "C" int luaopen_aelkey_usb(lua_State *L) {
  // clang-format off
  static const luaL_Reg funcs[] = {
    {"bulk_transfer", lua_bulk_transfer},
    {"control_transfer", lua_control_transfer},
    {"interrupt_transfer", lua_interrupt_transfer},
    {"submit_transfer", lua_submit_transfer},
    {nullptr, nullptr}
  };
  // clang-format on

  luaL_newlib(L, funcs);

  return 1;
}

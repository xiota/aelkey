#include "aelkey_hid.h"

#include <cstring>

#include <libusb-1.0/libusb.h>
#include <lua.hpp>

#include "aelkey_state.h"
#include "luacompat.h"

// Map libusb_transfer_type enum → string
static const char *transfer_type_to_string(uint8_t type) {
  switch (type) {
    case LIBUSB_TRANSFER_TYPE_CONTROL:
      return "control";
    case LIBUSB_TRANSFER_TYPE_ISOCHRONOUS:
      return "iso";
    case LIBUSB_TRANSFER_TYPE_BULK:
      return "bulk";
    case LIBUSB_TRANSFER_TYPE_INTERRUPT:
      return "interrupt";
    default:
      return "unknown";
  }
}

// Map libusb_transfer_status enum → string
static const char *transfer_status_to_string(libusb_transfer_status status) {
  switch (status) {
    case LIBUSB_TRANSFER_COMPLETED:
      return "ok";
    case LIBUSB_TRANSFER_ERROR:
      return "error";
    case LIBUSB_TRANSFER_TIMED_OUT:
      return "timeout";
    case LIBUSB_TRANSFER_CANCELLED:
      return "cancelled";
    case LIBUSB_TRANSFER_STALL:
      return "stall";
    case LIBUSB_TRANSFER_NO_DEVICE:
      return "no_device";
    case LIBUSB_TRANSFER_OVERFLOW:
      return "overflow";
    default:
      return "unknown";
  }
}

static void LIBUSB_CALL dispatch_libusb(libusb_transfer *transfer) {
  auto *ud = static_cast<std::pair<InputCtx *, lua_State *> *>(transfer->user_data);
  InputCtx *ctx = ud->first;
  lua_State *L = ud->second;

  if (!ctx->decl.callback_events.empty()) {
    lua_getglobal(L, ctx->decl.callback_events.c_str());
    if (lua_isfunction(L, -1)) {
      lua_newtable(L);

      lua_pushstring(L, ctx->decl.id.c_str());
      lua_setfield(L, -2, "device");

      lua_pushlstring(
          L, reinterpret_cast<const char *>(transfer->buffer), transfer->actual_length
      );
      lua_setfield(L, -2, "data");

      lua_pushinteger(L, transfer->actual_length);
      lua_setfield(L, -2, "size");

      lua_pushinteger(L, transfer->endpoint);
      lua_setfield(L, -2, "endpoint");

      lua_pushstring(L, transfer_type_to_string(transfer->type));
      lua_setfield(L, -2, "transfer");

      lua_pushstring(L, transfer_status_to_string(transfer->status));
      lua_setfield(L, -2, "status");

      if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
        std::cerr << "Lua libusb callback error: " << lua_tostring(L, -1) << std::endl;
        lua_pop(L, 1);
      }
    } else {
      lua_pop(L, 1);
    }
  }

  // resubmit for continuous streaming
  int rc = libusb_submit_transfer(transfer);
  if (rc != 0) {
    std::cerr << "libusb_submit_transfer error: " << libusb_error_name(rc) << std::endl;
  }
}

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
  lua_pushstring(L, dev_id);
  lua_setfield(L, -2, "device");
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

  // common fields
  lua_pushstring(L, dev_id);
  lua_setfield(L, -2, "device");
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
  lua_pushstring(L, dev_id);
  lua_setfield(L, -2, "device");
  lua_pushinteger(L, transferred);
  lua_setfield(L, -2, "size");
  lua_pushinteger(L, status);
  lua_setfield(L, -2, "status");

  return 1;
}

static int lua_submit_transfer(lua_State *L) {
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
  InputCtx *ctx = &it->second;

  // endpoint
  lua_getfield(L, 1, "endpoint");
  int endpoint = luaL_checkinteger(L, -1);
  lua_pop(L, 1);

  // type string
  lua_getfield(L, 1, "type");
  const char *type_str = luaL_checkstring(L, -1);
  lua_pop(L, 1);
  int type = LIBUSB_TRANSFER_TYPE_INTERRUPT;
  if (strcmp(type_str, "bulk") == 0) {
    type = LIBUSB_TRANSFER_TYPE_BULK;
  } else if (strcmp(type_str, "control") == 0) {
    type = LIBUSB_TRANSFER_TYPE_CONTROL;
  } else if (strcmp(type_str, "iso") == 0) {
    type = LIBUSB_TRANSFER_TYPE_ISOCHRONOUS;
  } else {
    type = LIBUSB_TRANSFER_TYPE_INTERRUPT;
  }

  // size
  lua_getfield(L, 1, "size");
  int size = luaL_checkinteger(L, -1);
  lua_pop(L, 1);

  // timeout (optional)
  lua_getfield(L, 1, "timeout");
  unsigned int timeout = lua_isnil(L, -1) ? 0u : (unsigned int)luaL_checkinteger(L, -1);
  lua_pop(L, 1);

  // allocate transfer + buffer
  libusb_transfer *xfer = libusb_alloc_transfer(0);
  if (!xfer) {
    lua_pushnil(L);
    return 1;
  }
  unsigned char *buf = (unsigned char *)malloc(size);
  if (!buf) {
    libusb_free_transfer(xfer);
    lua_pushnil(L);
    return 1;
  }

  // fill transfer
  xfer->dev_handle = handle;
  xfer->endpoint = (uint8_t)endpoint;
  xfer->type = (uint8_t)type;
  xfer->timeout = timeout;
  xfer->buffer = buf;
  xfer->length = size;
  // user_data carries InputCtx* and lua_State*
  xfer->user_data = new std::pair<InputCtx *, lua_State *>(ctx, L);
  xfer->callback = dispatch_libusb;

  int rc = libusb_submit_transfer(xfer);
  if (rc != 0) {
    std::cerr << "libusb_submit_transfer error: " << libusb_error_name(rc) << std::endl;
    free(buf);
    delete static_cast<std::pair<InputCtx *, lua_State *> *>(xfer->user_data);
    libusb_free_transfer(xfer);
    lua_pushnil(L);
    return 1;
  }

  // return a simple handle table with cancel/resubmit
  lua_newtable(L);

  lua_pushcfunction(L, [](lua_State *Lh) -> int {
    luaL_checktype(Lh, 1, LUA_TTABLE);
    lua_getfield(Lh, 1, "_xfer");
    libusb_transfer *x = (libusb_transfer *)lua_touserdata(Lh, -1);
    lua_pop(Lh, 1);
    if (x) {
      libusb_cancel_transfer(x);
    }
    lua_pushboolean(Lh, 1);
    return 1;
  });
  lua_setfield(L, -2, "cancel");

  lua_pushcfunction(L, [](lua_State *Lh) -> int {
    luaL_checktype(Lh, 1, LUA_TTABLE);
    lua_getfield(Lh, 1, "_xfer");
    libusb_transfer *x = (libusb_transfer *)lua_touserdata(Lh, -1);
    lua_pop(Lh, 1);
    if (!x) {
      lua_pushboolean(Lh, 0);
      return 1;
    }
    int rc = libusb_submit_transfer(x);
    lua_pushboolean(Lh, rc == 0);
    return 1;
  });
  lua_setfield(L, -2, "resubmit");

  // stash raw transfer pointer inside handle table
  lua_pushlightuserdata(L, xfer);
  lua_setfield(L, -2, "_xfer");

  return 1;
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

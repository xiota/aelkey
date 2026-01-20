#include "aelkey_usb.h"

#include <algorithm>
#include <cstring>
#include <vector>

#include <libusb-1.0/libusb.h>
#include <sol/sol.hpp>

#include "aelkey_hid.h"
#include "aelkey_state.h"
#include "device_udev.h"

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

static void destroy_transfer(libusb_transfer *t) {
  if (!t) {
    return;
  }

  if (t->buffer) {
    std::free(t->buffer);
    t->buffer = nullptr;
  }

  if (t->user_data) {
    delete static_cast<std::pair<InputCtx *, lua_State *> *>(t->user_data);
    t->user_data = nullptr;
  }

  libusb_free_transfer(t);
}

static void cleanup_transfer(InputCtx *ctx, libusb_transfer *transfer) {
  auto &vec = ctx->transfers;
  vec.erase(std::remove(vec.begin(), vec.end(), transfer), vec.end());
  destroy_transfer(transfer);
}

// libusb async callback → Lua
static void LIBUSB_CALL dispatch_libusb(libusb_transfer *transfer) {
  if (!transfer || !transfer->user_data) {
    return;
  }

  auto *ud = static_cast<std::pair<InputCtx *, lua_State *> *>(transfer->user_data);
  if (!ud) {
    destroy_transfer(transfer);
    return;
  }

  InputCtx *ctx = ud->first;
  lua_State *L = ud->second;

  sol::state_view lua(L);

  if (!ctx->decl.on_event.empty()) {
    sol::object cb_obj = lua[ctx->decl.on_event];
    if (cb_obj.is<sol::function>()) {
      sol::function cb = cb_obj.as<sol::function>();

      sol::table ev = lua.create_table();

      ev["device"] = ctx->decl.id;
      ev["data"] = std::string(
          reinterpret_cast<const char *>(transfer->buffer),
          static_cast<std::size_t>(transfer->actual_length)
      );
      ev["size"] = static_cast<int>(transfer->actual_length);
      ev["endpoint"] = static_cast<int>(transfer->endpoint);
      ev["transfer"] = transfer_type_to_string(transfer->type);
      ev["status"] = transfer_status_to_string(transfer->status);

      sol::protected_function pcb = cb;
      sol::protected_function_result r = pcb(ev);
      if (!r.valid()) {
        sol::error err = r;
        std::fprintf(stderr, "Lua libusb callback error: %s\n", err.what());
      }
    }
  }

  // resubmit transfer based on status
  switch (transfer->status) {
    case LIBUSB_TRANSFER_COMPLETED:
    case LIBUSB_TRANSFER_OVERFLOW:
    case LIBUSB_TRANSFER_TIMED_OUT: {
      // transient / normal: try resubmit
      if (libusb_submit_transfer(transfer) != 0) {
        cleanup_transfer(ctx, transfer);
      }
      break;
    }

    case LIBUSB_TRANSFER_NO_DEVICE:
      detach_input_device(ctx->decl.id);
      notify_state_change(L, ctx->decl, "remove");
      break;

    case LIBUSB_TRANSFER_CANCELLED:
    case LIBUSB_TRANSFER_ERROR:
    default: {
      libusb_device_descriptor desc;
      int rc = libusb_get_device_descriptor(libusb_get_device(ctx->usb_handle), &desc);
      if (rc != 0) {
        // device is gone
        detach_input_device(ctx->decl.id);
        notify_state_change(L, ctx->decl, "remove");
      } else {
        // fatal or cancelled, just clean up this transfer
        cleanup_transfer(ctx, transfer);
      }
      break;
    }
  }
}

// bulk_transfer{device, endpoint, size, [timeout]}
// Returns {device, data, size, status}
sol::object usb_bulk_transfer(sol::this_state ts, sol::table opts) {
  lua_State *L = ts;
  sol::state_view lua(L);

  // device id
  std::string dev_id = opts.get<std::string>("device");

  auto &state = AelkeyState::instance();
  auto it = state.input_map.find(dev_id);
  if (it == state.input_map.end() || !it->second.usb_handle) {
    sol::table result = lua.create_table();
    result["device"] = dev_id;
    result["data"] = "";
    result["size"] = 0;
    result["status"] = LIBUSB_ERROR_NO_DEVICE;
    return sol::make_object(lua, result);
  }
  libusb_device_handle *handle = it->second.usb_handle;

  // endpoint
  int endpoint = opts.get<int>("endpoint");

  // size
  int size = opts.get<int>("size");

  // timeout (optional, default 0)
  int timeout = opts.get_or("timeout", 0);

  bool is_in = (endpoint & LIBUSB_ENDPOINT_IN) != 0;

  int status = 0;
  int transferred = 0;

  sol::table result = lua.create_table();

  if (is_in) {
    // IN transfer: read from device
    std::vector<unsigned char> buf(size);
    status = libusb_bulk_transfer(handle, endpoint, buf.data(), size, &transferred, timeout);

    result["data"] = std::string(
        reinterpret_cast<const char *>(buf.data()), static_cast<std::size_t>(transferred)
    );
  } else {
    // OUT transfer: send data from Lua
    sol::optional<std::string> data_opt = opts.get<sol::optional<std::string>>("data");
    std::string out_data = data_opt.value_or(std::string());

    if (out_data.size() > static_cast<std::size_t>(size)) {
      out_data.resize(static_cast<std::size_t>(size));  // clamp to requested size
    }

    status = libusb_bulk_transfer(
        handle,
        endpoint,
        reinterpret_cast<unsigned char *>(out_data.data()),
        static_cast<int>(out_data.size()),
        &transferred,
        timeout
    );

    // echo back what was sent (matching original behavior)
    result["data"] = out_data.substr(0, static_cast<std::size_t>(transferred));
  }

  // common fields
  result["device"] = dev_id;
  result["size"] = transferred;
  result["status"] = status;

  return sol::make_object(lua, result);
}

// control_transfer{device, request_type, request, value, index, length, [timeout]}
// Returns {device, data, size, status}
sol::object usb_control_transfer(sol::this_state ts, sol::table opts) {
  lua_State *L = ts;
  sol::state_view lua(L);

  // device id
  std::string dev_id = opts.get<std::string>("device");

  auto &state = AelkeyState::instance();
  auto it = state.input_map.find(dev_id);
  if (it == state.input_map.end() || !it->second.usb_handle) {
    sol::table result = lua.create_table();
    result["device"] = dev_id;
    result["data"] = "";
    result["size"] = 0;
    result["status"] = LIBUSB_ERROR_NO_DEVICE;
    return sol::make_object(lua, result);
  }
  libusb_device_handle *handle = it->second.usb_handle;

  // request_type (bmRequestType)
  int request_type = opts.get<int>("request_type");

  // request (bRequest)
  int request = opts.get<int>("request");

  // value (wValue)
  int value = opts.get<int>("value");

  // index (wIndex)
  int index = opts.get<int>("index");

  // length (wLength)
  int length = opts.get<int>("length");

  // timeout (optional, default 0)
  int timeout = opts.get_or("timeout", 0);

  bool is_in = (request_type & LIBUSB_ENDPOINT_IN) != 0;

  int status = 0;
  int transferred = 0;

  sol::table result = lua.create_table();

  if (is_in) {
    // IN transfer: read from device
    std::vector<unsigned char> buf(static_cast<std::size_t>(length));
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

    result["data"] = std::string(
        reinterpret_cast<const char *>(buf.data()), static_cast<std::size_t>(transferred)
    );
  } else {
    // OUT transfer: send data from Lua
    sol::optional<std::string> data_opt = opts.get<sol::optional<std::string>>("data");
    std::string out_data = data_opt.value_or(std::string());

    std::size_t out_len = out_data.size();
    if (out_len > static_cast<std::size_t>(length)) {
      out_len = static_cast<std::size_t>(length);  // clamp to wLength
    }

    status = libusb_control_transfer(
        handle,
        static_cast<uint8_t>(request_type),
        static_cast<uint8_t>(request),
        static_cast<uint16_t>(value),
        static_cast<uint16_t>(index),
        reinterpret_cast<unsigned char *>(out_data.data()),
        static_cast<uint16_t>(out_len),
        static_cast<unsigned int>(timeout)
    );

    transferred = (status >= 0) ? status : 0;

    // original code returned an empty string for OUT
    result["data"] = "";
  }

  // common fields
  result["device"] = dev_id;
  result["size"] = transferred;
  result["status"] = status;

  return sol::make_object(lua, result);
}

// interrupt_transfer{device, endpoint, size, [timeout]}
// Returns {device, data, size, status}
sol::object usb_interrupt_transfer(sol::this_state ts, sol::table opts) {
  lua_State *L = ts;
  sol::state_view lua(L);

  // device id
  std::string dev_id = opts.get<std::string>("device");

  auto &state = AelkeyState::instance();
  auto it = state.input_map.find(dev_id);
  if (it == state.input_map.end() || !it->second.usb_handle) {
    sol::table result = lua.create_table();
    result["device"] = dev_id;
    result["data"] = "";
    result["size"] = 0;
    result["status"] = LIBUSB_ERROR_NO_DEVICE;
    return sol::make_object(lua, result);
  }
  libusb_device_handle *handle = it->second.usb_handle;

  // endpoint
  int endpoint = opts.get<int>("endpoint");

  // size
  int size = opts.get<int>("size");

  // timeout (optional, default 0)
  int timeout = opts.get_or("timeout", 0);

  bool is_in = (endpoint & LIBUSB_ENDPOINT_IN) != 0;

  int status = 0;
  int transferred = 0;

  sol::table result = lua.create_table();

  if (is_in) {
    // IN transfer: read from device
    std::vector<unsigned char> buf(size);
    status =
        libusb_interrupt_transfer(handle, endpoint, buf.data(), size, &transferred, timeout);

    result["data"] = std::string(
        reinterpret_cast<const char *>(buf.data()), static_cast<std::size_t>(transferred)
    );
  } else {
    // OUT transfer: send data from Lua
    sol::optional<std::string> data_opt = opts.get<sol::optional<std::string>>("data");
    std::string out_data = data_opt.value_or(std::string());

    if (out_data.size() > static_cast<std::size_t>(size)) {
      out_data.resize(static_cast<std::size_t>(size));
    }

    status = libusb_interrupt_transfer(
        handle,
        endpoint,
        reinterpret_cast<unsigned char *>(out_data.data()),
        static_cast<int>(out_data.size()),
        &transferred,
        timeout
    );

    // echo back what was sent
    result["data"] = out_data.substr(0, static_cast<std::size_t>(transferred));
  }

  // common fields
  result["device"] = dev_id;
  result["size"] = transferred;
  result["status"] = status;

  return sol::make_object(lua, result);
}

// submit_transfer{device, endpoint, type, size, [timeout]}
// Returns {device, endpoint, transfer, status}
sol::object usb_submit_transfer(sol::this_state ts, sol::table opts) {
  lua_State *L = ts;
  sol::state_view lua(L);

  // device id
  std::string dev_id = opts.get<std::string>("device");

  auto &state = AelkeyState::instance();
  auto it = state.input_map.find(dev_id);
  if (it == state.input_map.end() || !it->second.usb_handle) {
    sol::table result = lua.create_table();
    result["device"] = dev_id;
    result["endpoint"] = opts.get<int>("endpoint");
    result["transfer"] = sol::lua_nil;
    result["status"] = LIBUSB_ERROR_NO_DEVICE;
    return sol::make_object(lua, result);
  }

  libusb_device_handle *dev_handle = it->second.usb_handle;
  InputCtx *ctx = &it->second;

  // endpoint
  int endpoint = opts.get<int>("endpoint");

  // type string
  std::string type_str = opts.get<std::string>("type");
  int type = LIBUSB_TRANSFER_TYPE_INTERRUPT;
  if (type_str == "bulk") {
    type = LIBUSB_TRANSFER_TYPE_BULK;
  } else if (type_str == "control") {
    type = LIBUSB_TRANSFER_TYPE_CONTROL;
  } else if (type_str == "iso") {
    type = LIBUSB_TRANSFER_TYPE_ISOCHRONOUS;
  }

  // size
  int size = opts.get<int>("size");

  // timeout (optional)
  unsigned int timeout = static_cast<unsigned int>(opts.get_or("timeout", 0));

  // allocate transfer + buffer
  libusb_transfer *xfer = libusb_alloc_transfer(0);
  if (!xfer) {
    sol::table result = lua.create_table();
    result["device"] = dev_id;
    result["endpoint"] = endpoint;
    result["transfer"] = sol::lua_nil;
    result["status"] = LIBUSB_ERROR_NO_MEM;
    return sol::make_object(lua, result);
  }

  unsigned char *buf =
      static_cast<unsigned char *>(std::malloc(static_cast<std::size_t>(size)));
  if (!buf) {
    destroy_transfer(xfer);
    sol::table result = lua.create_table();
    result["device"] = dev_id;
    result["endpoint"] = endpoint;
    result["transfer"] = sol::lua_nil;
    result["status"] = LIBUSB_ERROR_NO_MEM;
    return sol::make_object(lua, result);
  }

  // fill transfer
  xfer->dev_handle = dev_handle;
  xfer->endpoint = static_cast<uint8_t>(endpoint);
  xfer->type = static_cast<uint8_t>(type);
  xfer->timeout = timeout;
  xfer->buffer = buf;
  xfer->length = size;
  xfer->user_data = new std::pair<InputCtx *, lua_State *>(ctx, L);
  xfer->callback = dispatch_libusb;

  int rc = libusb_submit_transfer(xfer);
  if (rc != 0) {
    std::fprintf(stderr, "libusb_submit_transfer error: %s\n", libusb_error_name(rc));
    destroy_transfer(xfer);

    sol::table result = lua.create_table();
    result["device"] = dev_id;
    result["endpoint"] = endpoint;
    result["transfer"] = sol::lua_nil;
    result["status"] = rc;
    return sol::make_object(lua, result);
  }

  // Track the transfer in the device context
  ctx->transfers.push_back(xfer);

  // return a simple handle table with cancel/resubmit
  sol::table t = lua.create_table();

  // store raw transfer pointer (preserve _xfer behavior)
  t["_xfer"] = sol::light(xfer);

  // cancel()
  t.set_function("cancel", [xfer]() {
    if (xfer) {
      libusb_cancel_transfer(xfer);
    }
    return true;
  });

  // resubmit()
  t.set_function("resubmit", [xfer]() {
    if (!xfer) {
      return false;
    }
    int rc2 = libusb_submit_transfer(xfer);
    return rc2 == 0;
  });

  return sol::make_object(lua, t);
}

extern "C" int luaopen_aelkey_usb(lua_State *L) {
  sol::state_view lua(L);

  sol::table mod = lua.create_table();

  mod.set_function("bulk_transfer", usb_bulk_transfer);
  mod.set_function("control_transfer", usb_control_transfer);
  mod.set_function("interrupt_transfer", usb_interrupt_transfer);
  mod.set_function("submit_transfer", usb_submit_transfer);

  return sol::stack::push(L, mod);
}

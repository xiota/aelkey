#include "lua_bindings/usb.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <sol/sol.hpp>

#include "backend_libusb.h"

// bulk_transfer{device, endpoint, size, [timeout]}
// Returns {device, data, size, status}
static sol::object usb_bulk_transfer(sol::this_state ts, sol::table opts) {
  lua_State *L = ts;
  sol::state_view lua(L);

  std::string dev_id = opts.get<std::string>("device");
  int endpoint = opts.get<int>("endpoint");
  int size = opts.get<int>("size");
  int timeout = opts.get_or("timeout", 0);

  bool is_in = (endpoint & 0x80) != 0;

  sol::optional<std::string> data_opt = opts.get<sol::optional<std::string>>("data");
  std::string out_data = data_opt.value_or(std::string());

  auto &backend = BackendLibUsb::instance();
  auto result = backend.bulk_transfer(dev_id, endpoint, size, timeout, is_in, out_data);

  sol::table t = lua.create_table();
  t["device"] = result.device;
  t["data"] = std::string_view(result.data.data(), result.data.size());
  t["size"] = result.size;
  t["status"] = result.status;
  return t;
}

// control_transfer{device, request_type, request, value, index, length, [timeout]}
// Returns {device, data, size, status}
static sol::object usb_control_transfer(sol::this_state ts, sol::table opts) {
  lua_State *L = ts;
  sol::state_view lua(L);

  std::string dev_id = opts.get<std::string>("device");
  int request_type = opts.get<int>("request_type");
  int request = opts.get<int>("request");
  int value = opts.get<int>("value");
  int index = opts.get<int>("index");
  int length = opts.get<int>("length");
  int timeout = opts.get_or("timeout", 0);

  bool is_in = (request_type & 0x80) != 0;

  sol::optional<std::string> data_opt = opts.get<sol::optional<std::string>>("data");
  std::string out_data = data_opt.value_or(std::string());

  auto &backend = BackendLibUsb::instance();
  auto result = backend.control_transfer(
      dev_id, request_type, request, value, index, length, timeout, is_in, out_data
  );

  sol::table t = lua.create_table();
  t["device"] = result.device;
  t["data"] = std::string_view(result.data.data(), result.data.size());
  t["size"] = result.size;
  t["status"] = result.status;
  return t;
}

// interrupt_transfer{device, endpoint, size, [timeout]}
// Returns {device, data, size, status}
static sol::object usb_interrupt_transfer(sol::this_state ts, sol::table opts) {
  lua_State *L = ts;
  sol::state_view lua(L);

  std::string dev_id = opts.get<std::string>("device");
  int endpoint = opts.get<int>("endpoint");
  int size = opts.get<int>("size");
  int timeout = opts.get_or("timeout", 0);

  bool is_in = (endpoint & 0x80) != 0;

  sol::optional<std::string> data_opt = opts.get<sol::optional<std::string>>("data");
  std::string out_data = data_opt.value_or(std::string());

  auto &backend = BackendLibUsb::instance();
  auto result = backend.interrupt_transfer(dev_id, endpoint, size, timeout, is_in, out_data);

  sol::table t = lua.create_table();
  t["device"] = result.device;
  t["data"] = std::string_view(result.data.data(), result.data.size());
  t["size"] = result.size;
  t["status"] = result.status;
  return t;
}

// submit_transfer{device, endpoint, type, size, [timeout]}
// Returns {device, endpoint, transfer, status}
static sol::object usb_submit_transfer(sol::this_state ts, sol::table opts) {
  lua_State *L = ts;
  sol::state_view lua(L);

  std::string dev_id = opts.get<std::string>("device");
  int endpoint = opts.get<int>("endpoint");
  std::string type_str = opts.get<std::string>("type");
  int size = opts.get<int>("size");
  int timeout = opts.get_or("timeout", 0);

  auto &backend = BackendLibUsb::instance();
  auto sr = backend.submit_transfer(dev_id, endpoint, type_str, size, timeout);

  sol::state_view lua_view(L);
  sol::table result = lua_view.create_table();

  if (!sr.handle || sr.status != "ok") {
    result["device"] = dev_id;
    result["endpoint"] = endpoint;
    result["transfer"] = sol::lua_nil;
    result["status"] = sr.status;
    return result;
  }

  sol::table t = lua.create_table();
  t["_xfer"] = sol::light(sr.handle);

  t.set_function("cancel", [h = sr.handle]() {
    return BackendLibUsb::instance().cancel_transfer(h);
  });

  t.set_function("resubmit", [h = sr.handle]() {
    return BackendLibUsb::instance().resubmit_transfer(h);
  });

  t["device"] = dev_id;
  t["endpoint"] = endpoint;
  t["status"] = sr.status;

  return sol::make_object(lua_view, t);
}

// clear_halt{device, endpoint}
// Returns {device, status}
static sol::object usb_clear_halt(sol::this_state ts, sol::table opts) {
  lua_State *L = ts;
  sol::state_view lua(L);

  std::string dev_id = opts.get<std::string>("device");
  int endpoint = opts.get<int>("endpoint");

  auto &backend = BackendLibUsb::instance();
  auto status = backend.clear_halt(dev_id, endpoint);

  sol::table t = lua.create_table();
  t["device"] = dev_id;
  t["status"] = status;
  return t;
}

// reset_device{device}
// Returns {device, status}
static sol::object usb_reset_device(sol::this_state ts, sol::table opts) {
  lua_State *L = ts;
  sol::state_view lua(L);

  std::string dev_id = opts.get<std::string>("device");

  auto &backend = BackendLibUsb::instance();
  auto status = backend.reset_device(dev_id);

  sol::table t = lua.create_table();
  t["device"] = dev_id;
  t["status"] = status;
  return t;
}

// set_configuration{device, config}
// Returns {device, status}
static sol::object usb_set_configuration(sol::this_state ts, sol::table opts) {
  lua_State *L = ts;
  sol::state_view lua(L);

  std::string dev_id = opts.get<std::string>("device");
  int config = opts.get<int>("config");

  auto &backend = BackendLibUsb::instance();
  auto status = backend.set_configuration(dev_id, config);

  sol::table t = lua.create_table();
  t["device"] = dev_id;
  t["status"] = status;
  return t;
}

// set_interface_alt_setting{device, interface, alt}
// Returns {device, status}
static sol::object usb_set_interface_alt_setting(sol::this_state ts, sol::table opts) {
  lua_State *L = ts;
  sol::state_view lua(L);

  std::string dev_id = opts.get<std::string>("device");
  int interface_number = opts.get<int>("interface");
  int alt_setting = opts.get<int>("alt");

  auto &backend = BackendLibUsb::instance();
  auto status = backend.set_interface_alt_setting(dev_id, interface_number, alt_setting);

  sol::table t = lua.create_table();
  t["device"] = dev_id;
  t["status"] = status;
  return t;
}

extern "C" int luaopen_aelkey_usb(lua_State *L) {
  sol::state_view lua(L);

  sol::table mod = lua.create_table();

  mod.set_function("bulk_transfer", usb_bulk_transfer);
  mod.set_function("control_transfer", usb_control_transfer);
  mod.set_function("interrupt_transfer", usb_interrupt_transfer);
  mod.set_function("submit_transfer", usb_submit_transfer);

  mod.set_function("clear_halt", usb_clear_halt);
  mod.set_function("reset_device", usb_reset_device);
  mod.set_function("set_configuration", usb_set_configuration);
  mod.set_function("set_interface_alt_setting", usb_set_interface_alt_setting);

  return sol::stack::push(L, mod);
}

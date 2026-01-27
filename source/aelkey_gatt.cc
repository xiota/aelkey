#include "aelkey_gatt.h"

#include <string>
#include <vector>

#include <sol/sol.hpp>

#include "aelkey_state.h"
#include "device_backend_gatt.h"

// gatt.read{ device="id", service=0x0021, characteristic=0x0036 }
// Returns raw data string
sol::object gatt_read(sol::this_state ts, sol::table opts) {
  lua_State *L = ts;
  sol::state_view lua(L);

  // device (required)
  std::string dev_id = opts.get<std::string>("device");

  // service (optional)
  int service = opts.get_or("service", -1);

  // characteristic (optional)
  int characteristic = opts.get_or("characteristic", -1);

  auto &gatt = DeviceBackendGATT::instance();
  std::string char_path = gatt.resolve_char_path(dev_id, service, characteristic);

  std::vector<uint8_t> data;
  bool ok = gatt.read_characteristic(char_path, data);

  if (!ok) {
    throw sol::error("GATT read failed");
  }

  return sol::make_object(
      lua, std::string(reinterpret_cast<const char *>(data.data()), data.size())
  );
}

// gatt.write{ device="id", data="...", response=true, service=0x0021, characteristic=0x0036 }
// Returns boolean success
sol::object gatt_write(sol::this_state ts, sol::table opts) {
  lua_State *L = ts;
  sol::state_view lua(L);

  // device (required)
  std::string dev_id = opts.get<std::string>("device");

  // data (required)
  std::string bytes = opts.get<std::string>("data");

  // response (optional)
  bool with_resp = opts.get_or("response", false);

  // service (optional)
  int service = opts.get_or("service", -1);

  // characteristic (optional)
  int characteristic = opts.get_or("characteristic", -1);

  auto &gatt = DeviceBackendGATT::instance();
  std::string char_path = gatt.resolve_char_path(dev_id, service, characteristic);

  bool ok = gatt.write_characteristic(
      char_path, reinterpret_cast<const uint8_t *>(bytes.data()), bytes.size(), with_resp
  );

  return sol::make_object(lua, ok);
}

extern "C" int luaopen_aelkey_gatt(lua_State *L) {
  sol::state_view lua(L);

  sol::table mod = lua.create_table();

  mod.set_function("read", gatt_read);
  mod.set_function("write", gatt_write);

  return sol::stack::push(L, mod);
}

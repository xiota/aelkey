#include "aelkey_gatt.h"

#include <string>
#include <vector>

#include <sol/sol.hpp>

#include "aelkey_state.h"
#include "device_gatt.h"

// Lookup InputCtx by device id
static InputCtx *get_ctx(sol::state_view lua, const std::string &dev_id) {
  auto &state = AelkeyState::instance();
  auto it = state.input_map.find(dev_id);
  if (it == state.input_map.end()) {
    throw sol::error("Unknown device id '" + dev_id + "'");
  }
  return &it->second;
}

// Resolve characteristic path using optional service/characteristic overrides
static std::string
resolve_char_path(sol::state_view lua, InputCtx *ctx, int service, int characteristic) {
  // No overrides → use primary characteristic
  if (service <= 0 && characteristic <= 0) {
    return ctx->decl.devnode;
  }

  // Overrides must both be provided
  if (service <= 0 || characteristic <= 0) {
    throw sol::error("GATT: both 'service' and 'characteristic' must be provided for override");
  }

  // Construct BlueZ object path:
  // /org/bluez/hci0/dev_xx/serviceXXXX/charYYYY
  char buf[256];
  std::snprintf(
      buf,
      sizeof(buf),
      "%s/service%04X/char%04X",
      ctx->gatt_path.c_str(),
      service,
      characteristic
  );

  return std::string(buf);
}

// gatt.read{ device="id", service=0x0021, characteristic=0x0036 }
// Returns raw data string
sol::object gatt_read(sol::this_state ts, sol::table opts) {
  lua_State *L = ts;
  sol::state_view lua(L);

  // device (required)
  std::string dev_id = opts.get<std::string>("device");
  InputCtx *ctx = get_ctx(lua, dev_id);

  // service (optional)
  int service = opts.get_or("service", -1);

  // characteristic (optional)
  int characteristic = opts.get_or("characteristic", -1);

  std::string char_path = resolve_char_path(lua, ctx, service, characteristic);

  std::vector<uint8_t> data;
  bool ok = gatt_read_characteristic(char_path, data);
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
  InputCtx *ctx = get_ctx(lua, dev_id);

  // data (required)
  std::string bytes = opts.get<std::string>("data");

  // response (optional)
  bool with_resp = opts.get_or("response", false);

  // service (optional)
  int service = opts.get_or("service", -1);

  // characteristic (optional)
  int characteristic = opts.get_or("characteristic", -1);

  std::string char_path = resolve_char_path(lua, ctx, service, characteristic);

  bool ok = gatt_write_characteristic(
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

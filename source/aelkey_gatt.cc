#include "aelkey_gatt.h"

#include <string>
#include <vector>

#include <lua.hpp>

#include "aelkey_state.h"
#include "device_gatt.h"
#include "luacompat.h"

// Lookup InputCtx by device id
static InputCtx *get_ctx(lua_State *L, const char *dev_id) {
  auto it = aelkey_state.input_map.find(dev_id);
  if (it == aelkey_state.input_map.end()) {
    luaL_error(L, "Unknown device id '%s'", dev_id);
    return nullptr;
  }
  return &it->second;
}

// Resolve characteristic path using optional service/characteristic overrides
static std::string
resolve_char_path(lua_State *L, InputCtx *ctx, int service, int characteristic) {
  // No overrides → use primary characteristic
  if (service <= 0 && characteristic <= 0) {
    return ctx->decl.devnode;
  }

  // Overrides must both be provided
  if (service <= 0 || characteristic <= 0) {
    luaL_error(L, "GATT: both 'service' and 'characteristic' must be provided for override");
    return {};
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
static int lua_gatt_read(lua_State *L) {
  luaL_checktype(L, 1, LUA_TTABLE);

  // device (required)
  lua_getfield(L, 1, "device");
  const char *dev_id = luaL_checkstring(L, -1);
  lua_pop(L, 1);

  InputCtx *ctx = get_ctx(L, dev_id);

  // service (optional)
  lua_getfield(L, 1, "service");
  int service = lua_isnil(L, -1) ? -1 : lua_tointeger(L, -1);
  lua_pop(L, 1);

  // characteristic (optional)
  lua_getfield(L, 1, "characteristic");
  int characteristic = lua_isnil(L, -1) ? -1 : lua_tointeger(L, -1);
  lua_pop(L, 1);

  std::string char_path = resolve_char_path(L, ctx, service, characteristic);

  std::vector<uint8_t> data;
  bool ok = gatt_read_characteristic(char_path, data);
  if (!ok) {
    return luaL_error(L, "GATT read failed");
  }

  lua_pushlstring(L, reinterpret_cast<const char *>(data.data()), data.size());
  return 1;
}

// gatt.write{ device="id", data="...", response=true, service=0x0021, characteristic=0x0036 }
static int lua_gatt_write(lua_State *L) {
  luaL_checktype(L, 1, LUA_TTABLE);

  // device (required)
  lua_getfield(L, 1, "device");
  const char *dev_id = luaL_checkstring(L, -1);
  lua_pop(L, 1);

  InputCtx *ctx = get_ctx(L, dev_id);

  // data (required)
  lua_getfield(L, 1, "data");
  size_t len = 0;
  const char *bytes = luaL_checklstring(L, -1, &len);
  lua_pop(L, 1);

  // response (optional)
  bool with_resp = false;
  lua_getfield(L, 1, "response");
  if (!lua_isnil(L, -1)) {
    with_resp = lua_toboolean(L, -1);
  }
  lua_pop(L, 1);

  // service (optional)
  lua_getfield(L, 1, "service");
  int service = lua_isnil(L, -1) ? -1 : lua_tointeger(L, -1);
  lua_pop(L, 1);

  // characteristic (optional)
  lua_getfield(L, 1, "characteristic");
  int characteristic = lua_isnil(L, -1) ? -1 : lua_tointeger(L, -1);
  lua_pop(L, 1);

  std::string char_path = resolve_char_path(L, ctx, service, characteristic);

  bool ok = gatt_write_characteristic(
      char_path, reinterpret_cast<const uint8_t *>(bytes), len, with_resp
  );

  lua_pushboolean(L, ok);
  return 1;
}

extern "C" int luaopen_aelkey_gatt(lua_State *L) {
  // clang-format off
  static const luaL_Reg funcs[] = {
    {"read",  lua_gatt_read},
    {"write", lua_gatt_write},
    {nullptr, nullptr}
  };
  // clang-format on

  luaL_newlib(L, funcs);
  return 1;
}

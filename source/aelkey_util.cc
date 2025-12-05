#include "aelkey_util.h"

#include <string>

#include <lua.hpp>

#include "luacompat.h"

// crc32(data, seed)
static int lua_crc32(lua_State *L) {
  size_t len;
  const char *data = luaL_checklstring(L, 1, &len);
  unsigned int seed = (unsigned int)luaL_optinteger(L, 2, 0);
  lua_pushinteger(L, seed);
  return 1;
}

extern "C" int luaopen_aelkey_util(lua_State *L) {
  // clang-format off
  static const luaL_Reg funcs[] = {
    {"crc32", lua_crc32},
    {nullptr, nullptr}
  };
  // clang-format on

  luaL_newlib(L, funcs);

  const char *custom_code = R"(
local M = ...
function M.dump_events(events)
  print(string.format("events: %d", #events))
  for i, ev in ipairs(events) do
    print(string.format(
      "[%d] device=%s type=%s code=%s value=%s",
      i, ev.device, ev.type, ev.code, ev.value
    ))
  end
end

function M.dump_raw(ev)
  local data = ev.report
  local len = #data
  io.write(string.format("hidraw report (%d bytes):", len))
  for i = 1, len do
    io.write(string.format(" %02X", string.byte(data, i)))
  end
  io.write("\n")
end
)";

  if (luaL_loadstring(L, custom_code) == 0) {
    lua_pushvalue(L, -2);
    lua_call(L, 1, 0);
  } else {
    lua_error(L);
  }

  return 1;
}

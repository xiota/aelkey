#include "aelkey_util.h"

#include <array>
#include <cstdint>
#include <string>

#include <lua.hpp>

#include "luacompat.h"

// Compute one CRC32 entry
constexpr uint32_t crc32_entry(int i) {
  uint32_t c = static_cast<uint32_t>(i);
  for (int j = 0; j < 8; ++j) {
    if (c & 1) {
      c = 0xEDB88320u ^ (c >> 1);
    } else {
      c >>= 1;
    }
  }
  return c;
}

// Generate full table at compile time
constexpr std::array<uint32_t, 256> make_crc32_table() {
  std::array<uint32_t, 256> table{};
  for (int i = 0; i < 256; ++i) {
    table[i] = crc32_entry(i);
  }
  return table;
}

constexpr auto crc32_table = make_crc32_table();

uint32_t crc32(const uint8_t *data, size_t len, uint32_t seed = 0) {
  uint32_t crc = ~seed;
  for (size_t i = 0; i < len; ++i) {
    crc = (crc >> 8) ^ crc32_table[(crc ^ data[i]) & 0xFF];
  }
  return ~crc;
}

// crc32(data, seed)
static int lua_crc32(lua_State *L) {
  size_t len;
  const char *data = luaL_checklstring(L, 1, &len);
  unsigned int seed = (unsigned int)luaL_optinteger(L, 2, 0);

  uint32_t result = crc32(reinterpret_cast<const uint8_t *>(data), len, seed);

  lua_pushinteger(L, result);
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

#include "aelkey_bit.h"

#include <cstdint>
#include <iomanip>
#include <sstream>

#include <lua.hpp>

#include "luacompat.h"

static inline uint32_t to_u32(lua_Integer x) {
  return static_cast<uint32_t>(x);
}

// bitwise AND of all arguments
static int lua_band(lua_State *L) {
  int n = lua_gettop(L);
  uint32_t r = to_u32(luaL_checkinteger(L, 1));
  for (int i = 2; i <= n; i++) {
    r &= to_u32(luaL_checkinteger(L, i));
  }
  lua_pushinteger(L, static_cast<lua_Integer>(r));
  return 1;
}

// bitwise OR of all arguments
static int lua_bor(lua_State *L) {
  int n = lua_gettop(L);
  uint32_t r = 0;
  for (int i = 1; i <= n; i++) {
    r |= to_u32(luaL_checkinteger(L, i));
  }
  lua_pushinteger(L, static_cast<lua_Integer>(r));
  return 1;
}

// bitwise XOR of all arguments
static int lua_bxor(lua_State *L) {
  int n = lua_gettop(L);
  uint32_t r = 0;
  for (int i = 1; i <= n; i++) {
    r ^= to_u32(luaL_checkinteger(L, i));
  }
  lua_pushinteger(L, static_cast<lua_Integer>(r));
  return 1;
}

// bitwise NOT of one argument
static int lua_bnot(lua_State *L) {
  uint32_t x = to_u32(luaL_checkinteger(L, 1));
  lua_pushinteger(L, static_cast<lua_Integer>(~x));
  return 1;
}

// logical left shift (mask count to 0–31)
static int lua_lshift(lua_State *L) {
  uint32_t x = to_u32(luaL_checkinteger(L, 1));
  int n = luaL_checkinteger(L, 2) & 31;  // mask to 0–31
  lua_pushinteger(L, static_cast<lua_Integer>(x << n));
  return 1;
}

// logical right shift (mask count to 0–31)
static int lua_rshift(lua_State *L) {
  uint32_t x = to_u32(luaL_checkinteger(L, 1));
  int n = luaL_checkinteger(L, 2) & 31;  // mask to 0–31
  lua_pushinteger(L, static_cast<lua_Integer>(x >> n));
  return 1;
}

// arithmetic right shift (preserves sign bit)
static int lua_arshift(lua_State *L) {
  int32_t x = static_cast<int32_t>(luaL_checkinteger(L, 1));
  int n = luaL_checkinteger(L, 2) & 31;  // mask to 0–31
  lua_pushinteger(L, static_cast<lua_Integer>(x >> n));
  return 1;
}

// rotate bits left by n (0–31)
static int lua_rol(lua_State *L) {
  uint32_t x = to_u32(luaL_checkinteger(L, 1));
  int n = luaL_checkinteger(L, 2) & 31;
  uint32_t r = (x << n) | (x >> (32 - n));
  lua_pushinteger(L, static_cast<lua_Integer>(r));
  return 1;
}

// rotate bits right by n (0–31)
static int lua_ror(lua_State *L) {
  uint32_t x = to_u32(luaL_checkinteger(L, 1));
  int n = luaL_checkinteger(L, 2) & 31;
  uint32_t r = (x >> n) | (x << (32 - n));
  lua_pushinteger(L, static_cast<lua_Integer>(r));
  return 1;
}

// Convert a 32‑bit integer to a hex string.
// Args:
//   1: value (forced into signed 32‑bit range via tobit)
//   2: width (optional, default = 8 digits, zero‑padded)
//   3: case flag (optional, -1 = lowercase [default], 1 = uppercase)
// Returns: string representation of the value in hex
static int lua_tohex(lua_State *L) {
  int32_t x = static_cast<int32_t>(luaL_checkinteger(L, 1));
  int width = luaL_optinteger(L, 2, 8);
  int caseflag = luaL_optinteger(L, 3, -1);  // -1 = lowercase, 1 = uppercase

  std::ostringstream oss;
  oss << std::hex << std::setw(width) << std::setfill('0');

  if (caseflag == 1) {
    oss << std::uppercase;
  } else {
    oss << std::nouppercase;
  }

  oss << static_cast<uint32_t>(x);
  lua_pushstring(L, oss.str().c_str());
  return 1;
}

// byte‑swap 32‑bit value (endian reversal)
static int lua_bswap(lua_State *L) {
  uint32_t x = to_u32(luaL_checkinteger(L, 1));
  uint32_t r = ((x & 0x000000FF) << 24) | ((x & 0x0000FF00) << 8) | ((x & 0x00FF0000) >> 8) |
               ((x & 0xFF000000) >> 24);
  lua_pushinteger(L, static_cast<lua_Integer>(r));
  return 1;
}

// force value into signed 32‑bit range
static int lua_tobit(lua_State *L) {
  int32_t x = static_cast<int32_t>(luaL_checkinteger(L, 1));
  lua_pushinteger(L, static_cast<lua_Integer>(x));
  return 1;
}

int luaopen_aelkey_bit(lua_State *L) {
  // If running under LuaJIT, return the built-in bit library
  lua_getglobal(L, "jit");
  if (!lua_isnil(L, -1)) {
    lua_pop(L, 1);  // pop jit
    // require("bit") and return it
    lua_getglobal(L, "require");
    lua_pushliteral(L, "bit");
    lua_call(L, 1, 1);
    return 1;
  }
  lua_pop(L, 1);  // pop nil

  // Otherwise return C implementation
  // clang-format off
  static const luaL_Reg funcs[] = {
    {"band", lua_band},
    {"bor", lua_bor},
    {"bxor", lua_bxor},
    {"bnot", lua_bnot},
    {"lshift", lua_lshift},
    {"rshift", lua_rshift},
    {"arshift", lua_arshift},
    {"rol", lua_rol},
    {"ror", lua_ror},
    {"tohex", lua_tohex},
    {"bswap", lua_bswap},
    {"tobit", lua_tobit},
    {nullptr, nullptr}
  };
  // clang-format on

  luaL_newlib(L, funcs);
  return 1;
}

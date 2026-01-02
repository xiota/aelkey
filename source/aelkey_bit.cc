#include "aelkey_bit.h"

#include <cstdint>
#include <iomanip>
#include <sstream>

#include <sol/sol.hpp>

uint32_t band(sol::variadic_args va) {
  uint32_t r = va.get<uint32_t>(0);
  for (size_t i = 1; i < va.size(); ++i) {
    r &= va.get<uint32_t>(i);
  }
  return r;
}

uint32_t bor(sol::variadic_args va) {
  uint32_t r = 0;
  for (auto v : va) {
    r |= v.as<uint32_t>();
  }
  return r;
}

uint32_t bxor(sol::variadic_args va) {
  uint32_t r = 0;
  for (auto v : va) {
    r ^= v.as<uint32_t>();
  }
  return r;
}

uint32_t bnot(uint32_t x) {
  return ~x;
}

uint32_t lshift(uint32_t x, int n) {
  return x << (n & 31);
}

uint32_t rshift(uint32_t x, int n) {
  return x >> (n & 31);
}

int32_t arshift(int32_t x, int n) {
  return x >> (n & 31);
}

uint32_t rol(uint32_t x, int n) {
  n &= 31;
  return (x << n) | (x >> (32 - n));
}

uint32_t ror(uint32_t x, int n) {
  n &= 31;
  return (x >> n) | (x << (32 - n));
}

std::string tohex(int32_t x, int width = 8, int caseflag = -1) {
  std::ostringstream oss;
  oss << std::hex << std::setw(width) << std::setfill('0');
  if (caseflag == 1) {
    oss << std::uppercase;
  }
  oss << static_cast<uint32_t>(x);
  return oss.str();
}

uint32_t bswap(uint32_t x) {
  return ((x & 0x000000FF) << 24) | ((x & 0x0000FF00) << 8) | ((x & 0x00FF0000) >> 8) |
         ((x & 0xFF000000) >> 24);
}

int32_t tobit(int32_t x) {
  return x;
}

extern "C" int luaopen_aelkey_bit(lua_State *L) {
  sol::state_view lua(L);

  // LuaJIT fallback: return the built-in bit module
  sol::object jit = lua["jit"];
  if (jit.valid() && jit.get_type() == sol::type::table) {
    sol::function require = lua["require"];
    sol::object bit = require("bit");
    return sol::stack::push(L, bit);
  }

  // Build module table
  sol::table mod = lua.create_table();

  mod.set_function("band", band);
  mod.set_function("bor", bor);
  mod.set_function("bxor", bxor);
  mod.set_function("bnot", bnot);
  mod.set_function("lshift", lshift);
  mod.set_function("rshift", rshift);
  mod.set_function("arshift", arshift);
  mod.set_function("rol", rol);
  mod.set_function("ror", ror);
  mod.set_function("tohex", tohex);
  mod.set_function("bswap", bswap);
  mod.set_function("tobit", tobit);

  return sol::stack::push(L, mod);
}

#include "aelkey_util.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <string>

#include <sol/sol.hpp>
#include <time.h>

#include "lua_scripts.h"

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
uint32_t util_crc32(const std::string &data, uint32_t seed = 0) {
  return crc32(reinterpret_cast<const uint8_t *>(data.data()), data.size(), seed);
}

// now("ms"|"us"|"ns")
uint64_t util_now(const std::string &unit = "ms") {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);

  if (unit == "us") {
    return ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000ULL;
  } else if (unit == "ns") {
    return ts.tv_sec * 1000000000ULL + ts.tv_nsec;
  } else {  // default ms
    return ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000ULL;
  }
}

extern "C" int luaopen_aelkey_util(lua_State *L) {
  sol::state_view lua(L);

  sol::table mod = lua.create_table();

  mod.set_function("crc32", util_crc32);
  mod.set_function("now", util_now);

  // Load script
  sol::load_result chunk = lua.load(aelkey_util_script);
  if (!chunk.valid()) {
    throw sol::error(
        "aelkey.util script load error: " + std::string(chunk.get<sol::error>().what())
    );
  }

  // Execute script with module table
  sol::protected_function_result result = chunk(mod);
  if (!result.valid()) {
    throw sol::error(
        "aelkey.util script runtime error: " + std::string(result.get<sol::error>().what())
    );
  }

  return sol::stack::push(L, mod);
}

#include "aelkey_midi.h"

#include <string>
#include <vector>

#include <sol/sol.hpp>

#include "device_out_midi.h"
#include "lua_scripts.h"

// midi.send("id", {0x90, 60, 127})
// midi.send("id", "\x90\x3C\x7F")
sol::object midi_send(sol::this_state ts, const std::string &id, sol::object bytes_obj) {
  lua_State *L = ts;
  sol::state_view lua(L);

  uint8_t buf[3];
  size_t len = 0;

  // Case 1: table of integers
  if (bytes_obj.is<sol::table>()) {
    sol::table tbl = bytes_obj.as<sol::table>();
    len = tbl.size();

    if (len == 0 || len > 3) {
      throw sol::error("MIDI message must contain 1 to 3 bytes");
    }

    for (size_t i = 1; i <= len; ++i) {
      int v = tbl.get<int>(i);
      if (v < 0 || v > 255) {
        throw sol::error("MIDI bytes must be integers 0–255");
      }
      buf[i - 1] = static_cast<uint8_t>(v);
    }
  }

  // Case 2: raw binary string
  else if (bytes_obj.is<std::string>()) {
    std::string raw = bytes_obj.as<std::string>();
    len = raw.size();

    if (len == 0 || len > 3) {
      throw sol::error("Raw MIDI string must contain 1 to 3 bytes");
    }

    for (size_t i = 0; i < len; ++i) {
      buf[i] = static_cast<uint8_t>(raw[i]);
    }
  }

  else {
    throw sol::error("midi.send expects a table of bytes or a raw string");
  }

  bool ok = DeviceOutMidi::instance().send(id, buf, len);
  return sol::make_object(lua, ok);
}

extern "C" int luaopen_aelkey_midi(lua_State *L) {
  sol::state_view lua(L);

  sol::table mod = lua.create_table();

  mod.set_function("send", midi_send);

  // Load script
  sol::load_result chunk = lua.load(aelkey_midi_script);
  if (!chunk.valid()) {
    throw sol::error(
        "aelkey.midi script load error: " + std::string(chunk.get<sol::error>().what())
    );
  }

  // Execute script with module table
  sol::protected_function_result result = chunk(mod);
  if (!result.valid()) {
    throw sol::error(
        "aelkey.midi script runtime error: " + std::string(result.get<sol::error>().what())
    );
  }

  return sol::stack::push(L, mod);
}

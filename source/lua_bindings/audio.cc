#include "lua_bindings/audio.h"

#include <string>
#include <vector>

#include <sol/sol.hpp>

#include "device_out_audio.h"

// audio.send("id", {0.1, 0.2, 0.3})
// audio.send("id", raw_float32_string)
static sol::object
audio_send(sol::this_state ts, const std::string &id, sol::object samples_obj) {
  lua_State *L = ts;
  sol::state_view lua(L);

  std::vector<float> buffer;
  const float *data_ptr = nullptr;
  size_t frames = 0;

  // Case 1: table of numbers
  if (samples_obj.is<sol::table>()) {
    sol::table tbl = samples_obj.as<sol::table>();
    size_t len = tbl.size();

    if (len == 0) {
      throw sol::error("audio.send: table must contain at least 1 sample");
    }

    buffer.resize(len);
    for (size_t i = 1; i <= len; ++i) {
      double v = tbl.get<double>(i);
      buffer[i - 1] = static_cast<float>(v);
    }

    data_ptr = buffer.data();
    frames = buffer.size();
  }

  // Case 2: raw binary string of float32 samples
  else if (samples_obj.is<std::string>()) {
    std::string raw = samples_obj.as<std::string>();

    if (raw.empty() || (raw.size() % sizeof(float)) != 0) {
      throw sol::error(
          "audio.send: raw string size must be a positive multiple of 4 (float32 samples)"
      );
    }

    frames = raw.size() / sizeof(float);

    buffer.resize(frames);
    std::memcpy(buffer.data(), raw.data(), raw.size());

    data_ptr = buffer.data();
  }

  else {
    throw sol::error("audio.send expects a table of numbers or a raw float32 string");
  }

  bool ok = DeviceOutAudio::instance().send(id, data_ptr, frames);
  return sol::make_object(lua, ok);
}

extern "C" int luaopen_aelkey_audio(lua_State *L) {
  sol::state_view lua(L);

  sol::table mod = lua.create_table();

  mod.set_function("send", audio_send);

  return sol::stack::push(L, mod);
}

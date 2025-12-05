#include <lua.hpp>

#include "aelkey_bit.h"
#include "aelkey_core.h"
#include "aelkey_loop.h"
#include "luacompat.h"

extern "C" int luaopen_aelkey(lua_State *L) {
  // clang-format off
  static const luaL_Reg funcs[] = {
    {"emit", lua_emit},
    {"run", lua_run},
    {"syn_report", lua_syn_report},
    {"tick", lua_tick},
    {nullptr, nullptr}
  };
  // clang-format on

  luaL_newlib(L, funcs);

  // bit submodule
  luaopen_aelkey_bit(L);
  lua_setfield(L, -2, "bit");

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

  // inject custom code
  if (luaL_loadstring(L, custom_code) == 0) {
    lua_pushvalue(L, -2);
    lua_call(L, 1, 0);
  } else {
    lua_error(L);
  }

  return 1;
}

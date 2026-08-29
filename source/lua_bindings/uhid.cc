#include "lua_bindings/uhid.h"

#include <sol/sol.hpp>

#include "device_out_uhid.h"

// reply(tbl)
// tbl: { dev_id = "...", trans_id = ..., status = "ok", data = "..." }
static sol::object uhid_reply(sol::this_state ts, sol::table tbl) {
  lua_State *L = ts;
  sol::state_view lua(L);

  std::string id = tbl["dev_id"];
  uint32_t trans_id = tbl["trans_id"].get_or<uint32_t>(0);
  std::string status = tbl["status"].get_or(std::string("ok"));
  std::string data = tbl["data"].get_or(std::string(""));

  DeviceOutUhid::instance().reply(id, trans_id, status, data);

  return sol::make_object(lua, true);
}

// write_report(tbl)
// tbl: { dev_id = "...", type = "input", data = "..." }
static sol::object uhid_write_report(sol::this_state ts, sol::table tbl) {
  lua_State *L = ts;
  sol::state_view lua(L);

  std::string id = tbl["dev_id"];
  std::string type = tbl["type"].get_or(std::string("input"));
  std::string data = tbl["data"].get_or(std::string(""));

  DeviceOutUhid::instance().write_report(id, type, data);

  return sol::make_object(lua, true);
}

extern "C" int luaopen_aelkey_uhid(lua_State *L) {
  sol::state_view lua(L);

  sol::table mod = lua.create_table();

  mod.set_function("reply", uhid_reply);
  mod.set_function("write_report", uhid_write_report);

  return sol::stack::push(L, mod);
}

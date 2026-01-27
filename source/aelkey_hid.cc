#include "aelkey_hid.h"

#include <vector>

#include <linux/hidraw.h>
#include <sol/sol.hpp>
#include <sys/ioctl.h>
#include <unistd.h>

#include "aelkey_state.h"

// get_feature_report(dev_id, report_id)
// Returns string (empty on failure)
sol::object hid_get_feature_report(sol::this_state ts, const std::string &id, int report_id) {
  lua_State *L = ts;
  sol::state_view lua(L);

  auto &state = AelkeyState::instance();
  auto it = state.input_map.find(id);
  if (it == state.input_map.end() || it->second.fd < 0) {
    return sol::make_object(lua, std::string());
  }

  InputDecl &decl = it->second;

  const int max_size = 256;
  std::vector<unsigned char> buf(max_size);
  buf[0] = static_cast<unsigned char>(report_id);

  if (ioctl(decl.fd, HIDIOCGFEATURE(max_size), buf.data()) < 0) {
    return sol::make_object(lua, std::string());
  }

  return sol::make_object(
      lua, std::string(reinterpret_cast<const char *>(buf.data()), max_size)
  );
}

// get_report_descriptor(dev_id)
// Returns string (empty on failure)
sol::object hid_get_report_descriptor(sol::this_state ts, const std::string &id) {
  lua_State *L = ts;
  sol::state_view lua(L);

  auto &state = AelkeyState::instance();
  auto it = state.input_map.find(id);
  if (it == state.input_map.end() || it->second.fd < 0) {
    return sol::make_object(lua, std::string());
  }

  InputDecl &decl = it->second;

  int desc_size;
  if (ioctl(decl.fd, HIDIOCGRDESCSIZE, &desc_size) < 0) {
    return sol::make_object(lua, std::string());
  }

  struct hidraw_report_descriptor rpt_desc;
  rpt_desc.size = desc_size;

  if (ioctl(decl.fd, HIDIOCGRDESC, &rpt_desc) < 0) {
    return sol::make_object(lua, std::string());
  }

  return sol::make_object(
      lua, std::string(reinterpret_cast<const char *>(rpt_desc.value), desc_size)
  );
}

// read_input_report(dev_id)
// Returns string (empty on failure)
sol::object hid_read_input_report(sol::this_state ts, const std::string &id) {
  lua_State *L = ts;
  sol::state_view lua(L);

  auto &state = AelkeyState::instance();
  auto it = state.input_map.find(id);
  if (it == state.input_map.end() || it->second.fd < 0) {
    return sol::make_object(lua, std::string());
  }

  InputDecl &decl = it->second;

  const int max_size = 256;
  std::vector<unsigned char> buf(max_size);

  ssize_t n = read(decl.fd, buf.data(), max_size);
  if (n <= 0) {
    return sol::make_object(lua, std::string());
  }

  return sol::make_object(lua, std::string(reinterpret_cast<const char *>(buf.data()), n));
}

// send_feature_report(dev_id, data)
// Returns boolean
sol::object
hid_send_feature_report(sol::this_state ts, const std::string &id, const std::string &data) {
  lua_State *L = ts;
  sol::state_view lua(L);

  auto &state = AelkeyState::instance();
  auto it = state.input_map.find(id);
  if (it == state.input_map.end() || it->second.fd < 0) {
    return sol::make_object(lua, false);
  }

  InputDecl &decl = it->second;

  if (ioctl(decl.fd, HIDIOCSFEATURE(data.size()), data.data()) < 0) {
    return sol::make_object(lua, false);
  }

  return sol::make_object(lua, true);
}

// send_output_report(dev_id, data)
// Returns boolean
sol::object
hid_send_output_report(sol::this_state ts, const std::string &id, const std::string &data) {
  lua_State *L = ts;
  sol::state_view lua(L);

  auto &state = AelkeyState::instance();
  auto it = state.input_map.find(id);
  if (it == state.input_map.end() || it->second.fd < 0) {
    return sol::make_object(lua, false);
  }

  InputDecl &decl = it->second;

  ssize_t n = write(decl.fd, data.data(), data.size());
  if (n < 0 || static_cast<size_t>(n) != data.size()) {
    return sol::make_object(lua, false);
  }

  return sol::make_object(lua, true);
}

extern "C" int luaopen_aelkey_hid(lua_State *L) {
  sol::state_view lua(L);

  sol::table mod = lua.create_table();

  mod.set_function("get_feature_report", hid_get_feature_report);
  mod.set_function("get_report_descriptor", hid_get_report_descriptor);
  mod.set_function("read_input_report", hid_read_input_report);
  mod.set_function("send_feature_report", hid_send_feature_report);
  mod.set_function("send_output_report", hid_send_output_report);

  return sol::stack::push(L, mod);
}

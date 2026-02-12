#include "lua_bindings/jack.h"

#include <string>
#include <unordered_map>
#include <vector>

#include <jack/jack.h>
#include <sol/sol.hpp>

#include "device_backend_jack.h"
#include "device_helpers.h"

// Short → full JACK type aliases
static const std::unordered_map<std::string, std::string> kJackTypeAliases = {
  { "audio", "32 bit float mono audio" },
  { "midi", "8 bit raw midi" },
  { "portaudio", "application/x-portaudio" },
  { "osc", "OSC" },
};

static std::string normalize_type(const sol::optional<std::string> &type_opt) {
  if (!type_opt) {
    return {};
  }

  std::string type = *type_opt;
  auto it = kJackTypeAliases.find(type);
  if (it != kJackTypeAliases.end()) {
    return it->second;
  }
  return type;
}

// jack.set_client_name(name) -> bool
static bool ak_jack_set_client_name(const std::string &name) {
  auto &jack = DeviceBackendJack::instance();

  // If client already exists, refuse to change name.
  if (jack.client() != nullptr) {
    return false;
  }

  // Backend: set desired client name to be used on first ensure_client().
  // You should implement this in DeviceBackendJack as:
  //   bool set_client_name(const std::string &name);
  return jack.set_client_name(name);
}

// jack.get_client_name() -> string|nil
static sol::object ak_jack_get_client_name(sol::this_state ts) {
  lua_State *L = ts;
  sol::state_view lua(L);

  auto &jack = DeviceBackendJack::instance();
  const std::string &name = jack.client_name();

  if (name.empty()) {
    return sol::nil;
  }

  return sol::make_object(lua, name);
}

// jack.list_ports([type], [flags]) -> { "Client:Port", ... }
static sol::object ak_jack_list_ports(
    sol::this_state ts,
    sol::optional<std::string> type_opt,
    sol::optional<int> flags_opt
) {
  lua_State *L = ts;
  sol::state_view lua(L);

  auto &jack = DeviceBackendJack::instance();

  std::string type = normalize_type(type_opt);
  unsigned long flags = 0;
  if (flags_opt) {
    flags = static_cast<unsigned long>(*flags_opt);
  }

  std::vector<std::string> ports =
      jack.list_ports(type.empty() ? nullptr : type.c_str(), flags);

  sol::table out = lua.create_table(static_cast<int>(ports.size()), 0);
  int idx = 1;
  for (auto &p : ports) {
    out[idx++] = p;
  }

  return out;
}

// jack.match_ports(pattern) -> { "Client:Port", ... } or {}
static sol::object ak_jack_match_ports(sol::this_state ts, const std::string &pattern) {
  lua_State *L = ts;
  sol::state_view lua(L);

  auto &jack = DeviceBackendJack::instance();

  std::vector<std::string> ports = jack.list_ports(nullptr, 0);
  sol::table results = lua.create_table();

  int idx = 1;
  for (auto &full : ports) {
    if (match_string(pattern, full)) {
      results[idx++] = full;
    }
  }

  return results;
}

// jack.connect(src, dst) -> bool
static bool ak_jack_connect_ports(const std::string &src, const std::string &dst) {
  auto &jack = DeviceBackendJack::instance();
  return jack.connect(src, dst);
}

// jack.disconnect(src, dst) -> bool
static bool ak_jack_disconnect_ports(const std::string &src, const std::string &dst) {
  auto &jack = DeviceBackendJack::instance();
  return jack.disconnect(src, dst);
}

// jack.get_port_info(port) -> table|nil
//
// Returns:
// {
//   name = "Client:Port",
//   type = "8 bit raw midi",
//   flags = {
//     input = true/false,
//     output = true/false,
//     physical = true/false,
//     terminal = true/false,
//     can_monitor = true/false,
//   },
//   aliases = { "alias1", "alias2", ... },
//   connections = { "OtherClient:Port", ... },
// }
static sol::object ak_jack_get_port_info(sol::this_state ts, const std::string &port_name) {
  lua_State *L = ts;
  sol::state_view lua(L);

  auto &jack = DeviceBackendJack::instance();

  if (!jack.ensure_client()) {
    return sol::nil;
  }

  jack_port_t *port = jack.find_port(port_name);
  if (!port) {
    return sol::nil;
  }

  sol::table info = lua.create_table();

  // Name
  const char *name = jack_port_name(port);
  info["name"] = name ? std::string{ name } : std::string{};

  // Type
  const char *type = jack_port_type(port);
  info["type"] = type ? std::string{ type } : std::string{};

  // Flags
  unsigned long flags = jack_port_flags(port);
  sol::table flags_tbl = lua.create_table();
  flags_tbl["input"] = static_cast<bool>(flags & JackPortIsInput);
  flags_tbl["output"] = static_cast<bool>(flags & JackPortIsOutput);
  flags_tbl["physical"] = static_cast<bool>(flags & JackPortIsPhysical);
  flags_tbl["terminal"] = static_cast<bool>(flags & JackPortIsTerminal);
  flags_tbl["can_monitor"] = static_cast<bool>(flags & JackPortCanMonitor);
  info["flags"] = flags_tbl;

  // Aliases
  {
    sol::table aliases_tbl = lua.create_table();

    size_t sz = jack_port_name_size();

    std::vector<char> buf1(sz);
    std::vector<char> buf2(sz);

    char *aliases[2] = { buf1.data(), buf2.data() };

    int alias_count = jack_port_get_aliases(port, aliases);

    int idx = 1;
    if (alias_count > 0 && buf1[0] != '\0') {
      aliases_tbl[idx++] = std::string(buf1.data());
    }
    if (alias_count > 1 && buf2[0] != '\0') {
      aliases_tbl[idx++] = std::string(buf2.data());
    }

    info["aliases"] = aliases_tbl;
  }

  // Connections
  {
    auto conns = jack.port_connections(port);

    sol::table conns_tbl = lua.create_table();
    int idx = 1;
    for (auto &c : conns) {
      conns_tbl[idx++] = c;
    }
    info["connections"] = conns_tbl;
  }

  return info;
}

extern "C" int luaopen_aelkey_jack(lua_State *L) {
  sol::state_view lua(L);

  sol::table mod = lua.create_table();

  mod.set_function("connect", ak_jack_connect_ports);
  mod.set_function("disconnect", ak_jack_disconnect_ports);
  mod.set_function("get_client_name", ak_jack_get_client_name);
  mod.set_function("get_port_info", ak_jack_get_port_info);
  mod.set_function("list_ports", ak_jack_list_ports);
  mod.set_function("match_ports", ak_jack_match_ports);
  mod.set_function("set_client_name", ak_jack_set_client_name);

  return sol::stack::push(L, mod);
}

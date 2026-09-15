#pragma once

#include <cstdio>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include <sol/sol.hpp>

#include <libevdev/libevdev.h>
#include <linux/input.h>

#include "utils/lua_helpers.h"

struct OutputCapability {
  std::string code;
  std::optional<int> min;
  std::optional<int> max;
  std::optional<int> fuzz;
  std::optional<int> flat;
  std::optional<int> resolution;

  static OutputCapability from_lua(sol::table t) {
    OutputCapability c;

    AelkeyUtil::lua_get_field(t, "code", c.code);
    AelkeyUtil::lua_get_field(t, "min", c.min);
    AelkeyUtil::lua_get_field(t, "max", c.max);
    AelkeyUtil::lua_get_field(t, "fuzz", c.fuzz);
    AelkeyUtil::lua_get_field(t, "flat", c.flat);
    AelkeyUtil::lua_get_field(t, "resolution", c.resolution);

    return c;
  }
};

struct InputDecl {
 public:
  std::string id;
  std::string type;

  std::vector<std::pair<int, int>> vid_pid;

  int bus = 0;
  int version = 0;

  std::vector<int> interfaces;
  std::string name;
  std::string phys;
  std::string uniq;

  bool grab = false;
  std::vector<std::pair<int, int>> capabilities;  // event_code, code_id
  std::vector<int> properties;                    // populated from capabilities table

  // gatt
  std::vector<int> services;
  std::vector<int> characteristics;

  std::string on_event;
  std::string on_state;

  // jack midi/audio
  std::string client;
  std::string port;

  // -- state --
  std::string devnode;
  int fd = -1;

  int vendor = 0;
  int product = 0;

  std::unordered_set<std::string> subbed_chars;

 public:
  sol::table to_lua(sol::state_view lua) const {
    sol::table t = lua.create_table();
    AelkeyUtil::lua_set_field(t, "id", id);
    AelkeyUtil::lua_set_field(t, "type", type);
    AelkeyUtil::lua_set_field(t, "vendor", vendor);
    AelkeyUtil::lua_set_field(t, "product", product);
    AelkeyUtil::lua_set_field(t, "version", version);
    AelkeyUtil::lua_set_field(t, "bus", bus);
    AelkeyUtil::lua_set_field(t, "name", name);
    AelkeyUtil::lua_set_field(t, "phys", phys);
    AelkeyUtil::lua_set_field(t, "uniq", uniq);
    AelkeyUtil::lua_set_field(t, "grab", grab);
    return t;
  }

  static InputDecl from_lua(sol::table t) {
    InputDecl d;
    AelkeyUtil::lua_get_field(t, "id", d.id);
    AelkeyUtil::lua_get_field(t, "type", d.type);
    AelkeyUtil::lua_get_field(t, "vendor", d.vendor);
    AelkeyUtil::lua_get_field(t, "product", d.product);
    AelkeyUtil::lua_get_field(t, "version", d.version);
    AelkeyUtil::lua_get_field(t, "name", d.name);
    AelkeyUtil::lua_get_field(t, "phys", d.phys);
    AelkeyUtil::lua_get_field(t, "uniq", d.uniq);
    AelkeyUtil::lua_get_field(t, "grab", d.grab);
    AelkeyUtil::lua_get_field(t, "on_event", d.on_event);
    AelkeyUtil::lua_get_field(t, "on_state", d.on_state);
    AelkeyUtil::lua_get_field(t, "client", d.client);
    AelkeyUtil::lua_get_field(t, "port", d.port);

    AelkeyUtil::lua_get_vector(t, "interfaces", d.interfaces);
    AelkeyUtil::lua_get_vector(t, "services", d.services);
    AelkeyUtil::lua_get_vector(t, "characteristics", d.characteristics);

    load_vid_pid(t["vid_pid"], d.vid_pid);
    load_serv_char(t["serv_char"], d.services, d.characteristics);
    load_bus(t["bus"], d.bus);
    load_capabilities(t["capabilities"], d);
    return d;
  }

 private:
  // vid_pid: array of { vendor, product }
  static void load_vid_pid(sol::object obj, std::vector<std::pair<int, int>> &out) {
    if (!obj.valid() || !obj.is<sol::table>()) {
      return;
    }

    obj.as<sol::table>().for_each([&](sol::object, sol::object value) {
      if (!value.is<sol::table>()) {
        return;
      }

      auto pair = value.as<sol::table>();

      int vendor = 0;
      int product = 0;

      AelkeyUtil::lua_get_field(pair, 1, vendor);
      AelkeyUtil::lua_get_field(pair, 2, product);

      out.emplace_back(vendor, product);
    });
  }

  // serv_char: array of { service, characteristic }
  // Flatten into services[] and characteristics[]
  static void load_serv_char(
      sol::object obj,
      std::vector<int> &services,
      std::vector<int> &characteristics
  ) {
    if (!obj.valid() || !obj.is<sol::table>()) {
      return;
    }

    obj.as<sol::table>().for_each([&](sol::object, sol::object value) {
      if (!value.is<sol::table>()) {
        return;
      }

      auto pair = value.as<sol::table>();

      int service = 0;
      int characteristic = 0;

      AelkeyUtil::lua_get_field(pair, 1, service);
      AelkeyUtil::lua_get_field(pair, 2, characteristic);

      if (service != 0) {
        services.push_back(service);
      }

      if (characteristic != 0) {
        characteristics.push_back(characteristic);
      }
    });
  }

  static void load_bus(sol::object obj, int &bus) {
    if (!obj.valid() || !obj.is<std::string>()) {
      return;
    }

    const auto value = obj.as<std::string>();

    if (value == "usb") {
      bus = BUS_USB;
    } else if (value == "bluetooth") {
      bus = BUS_BLUETOOTH;
    } else if (value == "pci") {
      bus = BUS_PCI;
    }
  }

  // capabilities: array of shorthand strings ("KEY_A")
  // or properties ("INPUT_PROP_POINTER")
  static void load_capabilities(sol::object obj, InputDecl &d) {
    if (!obj.valid() || !obj.is<sol::table>()) {
      return;
    }

    obj.as<sol::table>().for_each([&](sol::object, sol::object value) {
      if (!value.is<std::string>()) {
        return;
      }

      const std::string code = value.as<std::string>();

      if (code.rfind("INPUT_PROP_", 0) == 0) {
        int property = libevdev_property_from_name(code.c_str());

        if (property >= 0) {
          d.properties.push_back(property);
        } else {
          std::fprintf(stderr, "Unknown input property string: %s\n", code.c_str());
        }

        return;
      }

      int type = -1;

      if (code.rfind("KEY_", 0) == 0 || code.rfind("BTN_", 0) == 0) {
        type = EV_KEY;
      } else if (code.rfind("REL_", 0) == 0) {
        type = EV_REL;
      } else if (code.rfind("ABS_", 0) == 0) {
        type = EV_ABS;
      } else if (code.rfind("MSC_", 0) == 0) {
        type = EV_MSC;
      } else if (code.rfind("LED_", 0) == 0) {
        type = EV_LED;
      } else if (code.rfind("SND_", 0) == 0) {
        type = EV_SND;
      } else if (code.rfind("SW_", 0) == 0) {
        type = EV_SW;
      } else if (code.rfind("FF_", 0) == 0) {
        type = EV_FF;
      }

      if (type < 0) {
        return;
      }

      int event_code = libevdev_event_code_from_name(type, code.c_str());

      if (event_code >= 0) {
        d.capabilities.emplace_back(type, event_code);
      }
    });
  }
};

struct OutputDecl {
  std::string id;
  std::string type;

  int vendor = 0x1234;
  int product = 0x5678;
  int bus = 3;
  int version = 1;

  std::string name;
  std::string on_haptics;

  std::vector<OutputCapability> capabilities;

  std::string client;
  std::string port;

  std::string on_report;
  std::string report_desc;
  std::string phys;
  std::string uniq;
  int country = 0;

  static OutputDecl from_lua(sol::table t) {
    OutputDecl d;
    AelkeyUtil::lua_get_field(t, "id", d.id);
    AelkeyUtil::lua_get_field(t, "type", d.type);
    AelkeyUtil::lua_get_field(t, "vendor", d.vendor);
    AelkeyUtil::lua_get_field(t, "product", d.product);
    AelkeyUtil::lua_get_field(t, "version", d.version);
    AelkeyUtil::lua_get_field(t, "name", d.name);
    AelkeyUtil::lua_get_field(t, "on_haptics", d.on_haptics);
    AelkeyUtil::lua_get_field(t, "on_report", d.on_report);
    AelkeyUtil::lua_get_field(t, "report_desc", d.report_desc);
    AelkeyUtil::lua_get_field(t, "phys", d.phys);
    AelkeyUtil::lua_get_field(t, "uniq", d.uniq);
    AelkeyUtil::lua_get_field(t, "country", d.country);
    AelkeyUtil::lua_get_field(t, "client", d.client);
    AelkeyUtil::lua_get_field(t, "port", d.port);

    load_bus(t["bus"], d.bus);
    load_capabilities(t["capabilities"], d.capabilities);
    return d;
  }

 private:
  static void load_bus(sol::object obj, int &bus) {
    if (!obj.valid() || !obj.is<std::string>()) {
      return;
    }

    const auto value = obj.as<std::string>();

    if (value == "usb") {
      bus = BUS_USB;
    } else if (value == "bluetooth") {
      bus = BUS_BLUETOOTH;
    } else if (value == "pci") {
      bus = BUS_PCI;
    }
  }

  // capabilities: hybrid array of strings or configuration tables
  static void load_capabilities(sol::object obj, std::vector<OutputCapability> &out) {
    if (!obj.valid() || !obj.is<sol::table>()) {
      return;
    }

    obj.as<sol::table>().for_each([&](sol::object, sol::object value) {
      if (value.is<std::string>()) {
        OutputCapability c;
        c.code = value.as<std::string>();
        out.push_back(std::move(c));
      } else if (value.is<sol::table>()) {
        OutputCapability c = OutputCapability::from_lua(value.as<sol::table>());

        if (!c.code.empty()) {
          out.push_back(std::move(c));
        }
      }
    });
  }
};

#include "device_parser.h"

#include <climits>  // for PATH_MAX
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <glob.h>
#include <libevdev/libevdev.h>
#include <libudev.h>
#include <linux/hidraw.h>
#include <sol/sol.hpp>
#include <sys/epoll.h>
#include <sys/ioctl.h>

#include "aelkey_state.h"
#include "device_capabilities.h"
#include "dispatcher_haptics.h"

namespace DeviceParser {

// Parse a single InputDecl from a Lua table.
InputDecl parse_input(sol::table tbl) {
  InputDecl decl;

  // id
  if (sol::object v = tbl["id"]; v.valid() && v.is<std::string>()) {
    decl.id = v.as<std::string>();
  }

  // type
  if (sol::object v = tbl["type"]; v.valid() && v.is<std::string>()) {
    decl.type = v.as<std::string>();
  }

  // grab
  if (sol::object v = tbl["grab"]; v.valid() && v.is<bool>()) {
    decl.grab = v.as<bool>();
  }

  // vid_pid: array of { vendor, product }
  if (sol::object vp_obj = tbl["vid_pid"]; vp_obj.valid() && vp_obj.is<sol::table>()) {
    sol::table vp_tbl = vp_obj.as<sol::table>();
    vp_tbl.for_each([&](sol::object /*k*/, sol::object v) {
      if (!v.is<sol::table>()) {
        return;
      }
      sol::table pair_tbl = v.as<sol::table>();

      int vendor = 0;
      int product = 0;

      if (sol::object a = pair_tbl[1]; a.valid() && a.is<int>()) {
        vendor = a.as<int>();
      }
      if (sol::object b = pair_tbl[2]; b.valid() && b.is<int>()) {
        product = b.as<int>();
      }

      decl.vid_pid.emplace_back(vendor, product);
    });
  }

  // bus
  if (sol::object v = tbl["bus"]; v.valid() && v.is<std::string>()) {
    std::string busstr = v.as<std::string>();
    if (busstr == "usb") {
      decl.bus = BUS_USB;
    } else if (busstr == "bluetooth") {
      decl.bus = BUS_BLUETOOTH;
    } else if (busstr == "pci") {
      decl.bus = BUS_PCI;
    }
  }

  // interface
  if (sol::object v = tbl["interface"]; v.valid() && v.is<int>()) {
    decl.interface = v.as<int>();
  }

  // name
  if (sol::object v = tbl["name"]; v.valid() && v.is<std::string>()) {
    decl.name = v.as<std::string>();
  }

  // phys
  if (sol::object v = tbl["phys"]; v.valid() && v.is<std::string>()) {
    decl.phys = v.as<std::string>();
  }

  // uniq
  if (sol::object v = tbl["uniq"]; v.valid() && v.is<std::string>()) {
    decl.uniq = v.as<std::string>();
  }

  // capabilities: array of { type = "EV_KEY", code = "KEY_A" }
  if (sol::object caps_obj = tbl["capabilities"];
      caps_obj.valid() && caps_obj.is<sol::table>()) {
    sol::table caps = caps_obj.as<sol::table>();
    caps.for_each([&](sol::object /*k*/, sol::object v) {
      if (!v.is<sol::table>()) {
        return;
      }
      sol::table cap_tbl = v.as<sol::table>();

      std::string type_str;
      std::string code_str;

      if (sol::object t = cap_tbl["type"]; t.valid() && t.is<std::string>()) {
        type_str = t.as<std::string>();
      }
      if (sol::object c = cap_tbl["code"]; c.valid() && c.is<std::string>()) {
        code_str = c.as<std::string>();
      }

      if (!type_str.empty() && !code_str.empty()) {
        int type_id = libevdev_event_type_from_name(type_str.c_str());
        int code_id = libevdev_event_code_from_name(type_id, code_str.c_str());
        if (type_id >= 0 && code_id >= 0) {
          decl.capabilities.emplace_back(type_id, code_id);
        }
      }
    });
  }

  // services: array of ints
  if (sol::object s_obj = tbl["services"]; s_obj.valid()) {
    if (s_obj.is<sol::table>()) {
      sol::table s_tbl = s_obj.as<sol::table>();
      s_tbl.for_each([&](sol::object, sol::object v) {
        if (v.is<int>()) {
          decl.services.push_back(v.as<int>());
        }
      });
    }
  }

  // characteristics: array of ints
  if (sol::object c_obj = tbl["characteristics"]; c_obj.valid()) {
    if (c_obj.is<sol::table>()) {
      sol::table c_tbl = c_obj.as<sol::table>();
      c_tbl.for_each([&](sol::object, sol::object v) {
        if (v.is<int>()) {
          decl.characteristics.push_back(v.as<int>());
        }
      });
    }
  }

  // serv_char: array of { service, characteristic }
  // Flatten into services[] and characteristics[]
  if (sol::object sc_obj = tbl["serv_char"]; sc_obj.valid() && sc_obj.is<sol::table>()) {
    sol::table sc_tbl = sc_obj.as<sol::table>();
    sc_tbl.for_each([&](sol::object, sol::object v) {
      if (!v.is<sol::table>()) {
        return;
      }
      sol::table pair_tbl = v.as<sol::table>();

      int svc = 0;
      int chr = 0;

      if (sol::object a = pair_tbl[1]; a.valid() && a.is<int>()) {
        svc = a.as<int>();
      }
      if (sol::object b = pair_tbl[2]; b.valid() && b.is<int>()) {
        chr = b.as<int>();
      }

      if (svc != 0) {
        decl.services.push_back(svc);
      }
      if (chr != 0) {
        decl.characteristics.push_back(chr);
      }
    });
  }

  // on_event callback
  if (sol::object v = tbl["on_event"]; v.valid() && v.is<std::string>()) {
    decl.on_event = v.as<std::string>();
  }

  // on_state callback
  if (sol::object v = tbl["on_state"]; v.valid() && v.is<std::string>()) {
    decl.on_state = v.as<std::string>();
  }

  return decl;
}

OutputDecl parse_output(sol::table tbl) {
  OutputDecl decl;

  // id
  if (sol::object v = tbl["id"]; v.valid() && v.is<std::string>()) {
    decl.id = v.as<std::string>();
  }

  // type
  if (sol::object v = tbl["type"]; v.valid() && v.is<std::string>()) {
    decl.type = v.as<std::string>();
  }

  // profile
  if (sol::object v = tbl["profile"]; v.valid() && v.is<std::string>()) {
    decl.profile = v.as<std::string>();
  }

  // vendor
  if (sol::object v = tbl["vendor"]; v.valid() && v.is<int>()) {
    decl.vendor = v.as<int>();
  }

  // product
  if (sol::object v = tbl["product"]; v.valid() && v.is<int>()) {
    decl.product = v.as<int>();
  }

  // version
  if (sol::object v = tbl["version"]; v.valid() && v.is<int>()) {
    decl.version = v.as<int>();
  }

  // bus
  if (sol::object v = tbl["bus"]; v.valid() && v.is<std::string>()) {
    std::string busstr = v.as<std::string>();
    if (busstr == "usb") {
      decl.bus = BUS_USB;
    } else if (busstr == "bluetooth") {
      decl.bus = BUS_BLUETOOTH;
    } else if (busstr == "pci") {
      decl.bus = BUS_PCI;
    }
  }

  // name
  if (sol::object v = tbl["name"]; v.valid() && v.is<std::string>()) {
    decl.name = v.as<std::string>();
  }

  // haptics callback
  if (sol::object v = tbl["on_haptics"]; v.valid() && v.is<std::string>()) {
    decl.on_haptics = v.as<std::string>();
  }

  // capabilities
  if (sol::object caps_obj = tbl["capabilities"];
      caps_obj.valid() && caps_obj.is<sol::table>()) {
    sol::table caps = caps_obj.as<sol::table>();
    caps.for_each([&](sol::object /*k*/, sol::object v) {
      if (v.is<std::string>()) {
        decl.capabilities.push_back(v.as<std::string>());
      }
    });
  }

  return decl;
}

}  // namespace DeviceParser

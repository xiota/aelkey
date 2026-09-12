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

  // interfaces: array of ints
  if (sol::object v = tbl["interfaces"]; v.valid() && v.is<sol::table>()) {
    sol::table if_tbl = v.as<sol::table>();
    if_tbl.for_each([&](sol::object, sol::object val) {
      if (val.is<int>()) {
        decl.interfaces.push_back(val.as<int>());
      }
    });
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

  // phys
  if (sol::object v = tbl["phys"]; v.valid() && v.is<std::string>()) {
    decl.phys = v.as<std::string>();
  }

  // uniq
  if (sol::object v = tbl["uniq"]; v.valid() && v.is<std::string>()) {
    decl.uniq = v.as<std::string>();
  }

  // capabilities: array of shorthand strings ("KEY_A")
  // or properties ("INPUT_PROP_POINTER")
  if (sol::object caps_obj = tbl["capabilities"];
      caps_obj.valid() && caps_obj.is<sol::table>()) {
    sol::table caps = caps_obj.as<sol::table>();
    caps.for_each([&](sol::object /*k*/, sol::object v) {
      if (!v.is<std::string>()) {
        return;
      }
      std::string code_str = v.as<std::string>();

      if (code_str.rfind("INPUT_PROP_", 0) == 0) {
        int prop_id = libevdev_property_from_name(code_str.c_str());
        if (prop_id >= 0) {
          decl.properties.push_back(prop_id);
        } else {
          std::fprintf(stderr, "Unknown input property string: %s\n", code_str.c_str());
        }
        return;
      }

      int type_id = -1;

      if (code_str.rfind("KEY_", 0) == 0 || code_str.rfind("BTN_", 0) == 0) {
        type_id = EV_KEY;
      } else if (code_str.rfind("REL_", 0) == 0) {
        type_id = EV_REL;
      } else if (code_str.rfind("ABS_", 0) == 0) {
        type_id = EV_ABS;
      } else if (code_str.rfind("MSC_", 0) == 0) {
        type_id = EV_MSC;
      } else if (code_str.rfind("LED_", 0) == 0) {
        type_id = EV_LED;
      } else if (code_str.rfind("SND_", 0) == 0) {
        type_id = EV_SND;
      } else if (code_str.rfind("SW_", 0) == 0) {
        type_id = EV_SW;
      } else if (code_str.rfind("FF_", 0) == 0) {
        type_id = EV_FF;
      }

      if (type_id >= 0) {
        int code_id = libevdev_event_code_from_name(type_id, code_str.c_str());
        if (code_id >= 0) {
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

  // uhid report callback
  if (sol::object v = tbl["on_report"]; v.valid() && v.is<std::string>()) {
    decl.on_report = v.as<std::string>();
  }

  // uhid report descriptor
  if (sol::object v = tbl["report_desc"]; v.valid() && v.is<std::string>()) {
    decl.report_desc = v.as<std::string>();
  }

  // uhid phys
  if (sol::object v = tbl["phys"]; v.valid() && v.is<std::string>()) {
    decl.phys = v.as<std::string>();
  }

  // uhid uniq
  if (sol::object v = tbl["uniq"]; v.valid() && v.is<std::string>()) {
    decl.uniq = v.as<std::string>();
  }

  // uhid country
  if (sol::object v = tbl["country"]; v.valid() && v.is<int>()) {
    decl.country = v.as<int>();
  }

  // capabilities: hybrid array of strings or configuration tables
  if (sol::object caps_obj = tbl["capabilities"];
      caps_obj.valid() && caps_obj.is<sol::table>()) {
    sol::table caps = caps_obj.as<sol::table>();
    caps.for_each([&](sol::object /*k*/, sol::object v) {
      if (v.is<std::string>()) {
        // Shorthand string (e.g., "BTN_SOUTH")
        OutputCapability cap;
        cap.code = v.as<std::string>();
        decl.capabilities.push_back(std::move(cap));
      } else if (v.is<sol::table>()) {
        // Detailed configuration table (e.g., { code = "ABS_X", min = -32767, max = 32767 })
        sol::table cap_tbl = v.as<sol::table>();
        OutputCapability cap;

        if (sol::object c = cap_tbl["code"]; c.valid() && c.is<std::string>()) {
          cap.code = c.as<std::string>();
        }

        if (!cap.code.empty()) {
          if (sol::object m = cap_tbl["min"]; m.valid() && m.is<int>()) {
            cap.min = m.as<int>();
          }
          if (sol::object m = cap_tbl["max"]; m.valid() && m.is<int>()) {
            cap.max = m.as<int>();
          }
          if (sol::object f = cap_tbl["fuzz"]; f.valid() && f.is<int>()) {
            cap.fuzz = f.as<int>();
          }
          if (sol::object f = cap_tbl["flat"]; f.valid() && f.is<int>()) {
            cap.flat = f.as<int>();
          }
          if (sol::object r = cap_tbl["resolution"]; r.valid() && r.is<int>()) {
            cap.resolution = r.as<int>();
          }

          decl.capabilities.push_back(std::move(cap));
        }
      }
    });
  }

  return decl;
}

}  // namespace DeviceParser

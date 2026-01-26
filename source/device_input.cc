#include "device_input.h"

#include <climits>  // for PATH_MAX
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>

#include <fcntl.h>
#include <glob.h>
#include <libevdev/libevdev-uinput.h>
#include <libevdev/libevdev.h>
#include <libudev.h>
#include <linux/hidraw.h>
#include <sol/sol.hpp>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "aelkey_state.h"
#include "device_helpers.h"
#include "device_manager.h"
#include "dispatcher_evdev.h"
#include "dispatcher_gatt.h"
#include "dispatcher_hidraw.h"
#include "dispatcher_libusb.h"
#include "dispatcher_registry.h"
#include "dispatcher_udev.h"

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

  // vendor
  if (sol::object v = tbl["vendor"]; v.valid() && v.is<int>()) {
    decl.vendor = v.as<int>();
  }

  // product
  if (sol::object v = tbl["product"]; v.valid() && v.is<int>()) {
    decl.product = v.as<int>();
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

  // service
  if (sol::object v = tbl["service"]; v.valid() && v.is<int>()) {
    decl.service = v.as<int>();
  }

  // characteristic
  if (sol::object v = tbl["characteristic"]; v.valid() && v.is<int>()) {
    decl.characteristic = v.as<int>();
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

std::string match_device(const InputDecl &decl) {
  std::string devnode;
  if (DeviceManager::instance().match(decl, devnode)) {
    return devnode;  // backend matched successfully
  }

  return {};
}

void parse_inputs_from_lua(sol::this_state ts) {
  sol::state_view lua(ts);

  auto &state = AelkeyState::instance();
  state.input_decls.clear();

  sol::object obj = lua["inputs"];
  if (!obj.valid() || !obj.is<sol::table>()) {
    return;
  }

  sol::table inputs = obj.as<sol::table>();

  inputs.for_each([&](sol::object /*k*/, sol::object v) {
    if (v.is<sol::table>()) {
      InputDecl decl = parse_input(v.as<sol::table>());
      if (!decl.id.empty()) {
        state.input_decls.push_back(decl);
      }
    }
  });
}

bool attach_input_device(const std::string &devnode, const InputDecl &decl) {
  if (DeviceManager::instance().attach(decl, devnode)) {
    return true;
  }

  return false;
}

InputDecl detach_input_device(const std::string &dev_id) {
  if (auto maybe_decl = DeviceManager::instance().detach(dev_id)) {
    return *maybe_decl;
  }

  return {};
}

#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include <libevdev/libevdev.h>
#include <libusb-1.0/libusb.h>
#include <sol/sol.hpp>

#include "haptics_context.h"

struct InputDecl {
  std::string id;
  std::string type;
  int vendor = 0;
  int product = 0;
  int bus = 0;
  int interface = -1;
  std::string name;
  std::string phys;
  std::string uniq;

  bool grab = false;
  std::vector<std::pair<int, int>> capabilities;

  int service = 0;
  int characteristic = 0;

  std::string devnode;

  std::string on_event;  // HID input events
  std::string on_state;  // lifecycle events
};

struct InputCtx {
  InputDecl decl;
  libevdev *idev = nullptr;
  int fd = -1;
  libusb_device_handle *usb_handle = nullptr;
  std::vector<libusb_transfer *> transfers;

  bool grab_pending = false;
  bool grabbed = false;

  // The BlueZ device path, e.g. "/org/bluez/hci0/dev_XX_XX_XX_XX_XX_XX"
  // This is the root under which all GATT services/characteristics live.
  std::string gatt_path;

  HapticsSinkCtx haptics;
};

// Parse a single input declaration from a Lua table.
InputDecl parse_input(sol::table tbl);

// Parse global "inputs" table from the given Lua state and fill aelkey_state.input_decls.
void parse_inputs_from_lua(sol::this_state ts);

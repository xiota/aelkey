#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include <libevdev/libevdev.h>
#include <libusb-1.0/libusb.h>
#include <lua.hpp>

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
  bool writable = false;
  bool grab = false;
  std::vector<std::pair<int, int>> capabilities;

  int service = 0;
  int characteristic = 0;

  std::string devnode;

  std::string callback_events;  // HID input events
  std::string callback_state;   // lifecycle events
};

struct InputCtx {
  InputDecl decl;
  libevdev *idev = nullptr;
  int fd = -1;
  libusb_device_handle *usb_handle = nullptr;
  std::vector<libusb_transfer *> transfers;

  bool active = false;

  // The BlueZ device path, e.g. "/org/bluez/hci0/dev_XX_XX_XX_XX_XX_XX"
  // This is the root under which all GATT services/characteristics live.
  std::string gatt_path;
};

InputDecl parse_input(lua_State *L, int index);

std::string match_device(const InputDecl &decl);

void parse_inputs_from_lua(lua_State *L);
InputDecl detach_input_device(const std::string &dev_id);

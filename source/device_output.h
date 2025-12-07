#pragma once

#include <string>
#include <vector>

#include <libevdev/libevdev-uinput.h>
#include <lua.hpp>

struct OutputDecl {
  std::string id;
  std::string type;
  int vendor = 0x1234;
  int product = 0x5678;
  int bus = 3;
  int version = 1;
  std::string name;
  std::vector<std::string> capabilities;
};

libevdev_uinput *create_output_device(const OutputDecl &out);
void parse_outputs_from_lua(lua_State *L);

#pragma once

#include <string>

#include <libevdev/libevdev-uinput.h>
#include <lua.hpp>

struct OutputDecl {
  std::string id;
  std::string type;
  std::string name;
};

libevdev_uinput *create_output_device(const OutputDecl &out);
void parse_outputs_from_lua(lua_State *L);

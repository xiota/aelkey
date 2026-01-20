#pragma once

#include <string>
#include <vector>

#include <libevdev/libevdev-uinput.h>
#include <sol/sol.hpp>

struct OutputDecl {
  std::string id;
  std::string type;
  int vendor = 0x1234;
  int product = 0x5678;
  int bus = 3;
  int version = 1;
  std::string name;
  std::string on_haptics;
  std::vector<std::string> capabilities;
};

libevdev_uinput *create_output_device(const OutputDecl &out);

// Parse global "outputs" table from the given Lua state.
// Fills aelkey_state.output_decls.
void parse_outputs_from_lua(sol::this_state ts);

// Parse a single output declaration from a Lua table.
OutputDecl parse_output(sol::table tbl);

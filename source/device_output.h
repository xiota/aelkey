#pragma once

#include <sol/sol.hpp>

#include <libevdev/libevdev-uinput.h>

#include "device_declarations.h"

libevdev_uinput *create_output_device(const OutputDecl &out);

// Parse global "outputs" table from the given Lua state.
// Fills aelkey_state.output_decls.
void parse_outputs_from_lua(sol::this_state ts);

// Parse a single output declaration from a Lua table.
OutputDecl parse_output(sol::table tbl);

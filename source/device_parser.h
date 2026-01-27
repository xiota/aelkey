#pragma once

#include <sol/sol.hpp>

#include "device_declarations.h"

namespace DeviceParser {

// Parse a single input declaration from a Lua table.
InputDecl parse_input(sol::table tbl);

// Parse a single output declaration from a Lua table.
OutputDecl parse_output(sol::table tbl);

}  // namespace AelkeyParser

#pragma once

#include <sol/sol.hpp>

#include "device_declarations.h"

// Parse a single input declaration from a Lua table.
InputDecl parse_input(sol::table tbl);

// Parse global "inputs" table from the given Lua state and fill aelkey_state.input_decls.
void parse_inputs_from_lua(sol::this_state ts);

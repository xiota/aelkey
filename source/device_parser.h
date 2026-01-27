#pragma once

#include <sol/sol.hpp>

#include "device_declarations.h"

// Parse a single input declaration from a Lua table.
InputDecl parse_input(sol::table tbl);

// Parse global "inputs" table from the given Lua state.
// Fill aelkey_state.input_decls.
void parse_inputs_from_lua(sol::this_state ts);

// Parse global "outputs" table from the given Lua state.
// Fill aelkey_state.output_decls.
void parse_outputs_from_lua(sol::this_state ts);

// Parse a single output declaration from a Lua table.
OutputDecl parse_output(sol::table tbl);

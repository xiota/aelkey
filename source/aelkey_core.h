#pragma once

#include <sol/sol.hpp>

sol::object core_tick(sol::this_state ts, int ms, sol::object cb_obj);

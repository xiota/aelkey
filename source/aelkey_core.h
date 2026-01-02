#pragma once

#include <filesystem>
#include <string>

#include <sol/sol.hpp>

sol::object core_emit(sol::this_state ts, sol::table opts);
sol::object core_syn_report(sol::this_state ts, sol::optional<std::string> dev_id);
sol::object core_tick(sol::this_state ts, int ms, sol::object cb_obj);

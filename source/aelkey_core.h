#pragma once

#include <string>

#include <sol/sol.hpp>

sol::object core_open_device(sol::this_state ts, sol::optional<std::string> dev_id_opt);

sol::object core_close_device(sol::this_state ts, const std::string &dev_id);

sol::object core_get_device_info(sol::this_state ts, const std::string &dev_id);

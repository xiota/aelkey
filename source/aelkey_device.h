#pragma once

#include <sol/sol.hpp>
#include <string>

// Open all devices declared in the global "inputs" and "outputs" tables.
// If called with no arguments: open all devices.
// If called with a device ID: open only that device.
sol::object device_open(sol::this_state ts, sol::optional<std::string> dev_id);

// Close a device by ID.
sol::object device_close(sol::this_state ts, const std::string &dev_id);

// Return a table describing a device (id, type, vendor, product, etc).
sol::object device_get_info(sol::this_state ts, const std::string &dev_id);

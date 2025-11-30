#pragma once

#include <string>
#include <unordered_map>

#include <libevdev/libevdev-uinput.h>

#include "lua_bindings.h"

extern int epfd;

extern std::unordered_map<std::string, libevdev_uinput *> uinput_devices;

extern std::unordered_map<int, TickCallback> tick_callbacks;

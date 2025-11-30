#pragma once

#include <string>
#include <unordered_map>

#include <libevdev/libevdev-uinput.h>

extern int tfd;
extern int epfd;
extern int tick_payload_ref;

extern std::unordered_map<std::string, libevdev_uinput *> uinput_devices;

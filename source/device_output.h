#pragma once

#include <libevdev/libevdev-uinput.h>

#include "device_declarations.h"

// Create virtual devices
libevdev_uinput *create_output_device(const OutputDecl &out);

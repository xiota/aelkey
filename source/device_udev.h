#pragma once

#include <string>

#include <libudev.h>
#include <sol/sol.hpp>

#include "device_input.h"

bool attach_input_device(const std::string &devnode, const InputDecl &decl);

void ensure_udev_initialized(sol::this_state ts);
void notify_state_change(sol::this_state ts, const InputDecl &decl, const char *state);

void handle_udev_add(sol::this_state ts, struct udev_device *dev);
void handle_udev_remove(sol::this_state ts, struct udev_device *dev);

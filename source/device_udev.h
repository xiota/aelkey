#pragma once

#include <string>

#include <libudev.h>
#include <lua.hpp>

#include "device_input.h"

bool attach_input_device(const std::string &devnode, const InputDecl &decl);

int device_udev_init(lua_State *L);
void notify_state_change(lua_State *L, const InputDecl &decl, const char *state);

void handle_udev_add(lua_State *L, struct udev_device *dev);
void handle_udev_remove(lua_State *L, struct udev_device *dev);

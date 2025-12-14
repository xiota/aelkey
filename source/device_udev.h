#pragma once

#include <string>

#include <lua.hpp>

#include "device_input.h"

bool attach_input_device(const std::string &devnode, const InputDecl &decl);

int device_udev_init(lua_State *L);
void notify_state_change(lua_State *L, const InputDecl &decl, const char *state);

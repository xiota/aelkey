#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include <libevdev/libevdev.h>
#include <lua.hpp>

#include "device_output.h"

struct InputDecl {
  std::string id;
  std::string kind;
  int vendor = 0;
  int product = 0;
  int bus = 0;
  std::string name;
  std::string phys;
  std::string uniq;
  bool writable = false;
  bool grab = false;
  std::vector<std::pair<int, int>> capabilities;
  std::string callback;
};

struct InputCtx {
  InputDecl decl;
  libevdev *idev = nullptr;
};

InputDecl parse_input(lua_State *L, int index);

OutputDecl parse_output(lua_State *L, int index);

std::string match_device(const InputDecl &decl);

int attach_device(
    const std::string &devnode,
    const InputDecl &in,
    std::unordered_map<int, InputCtx> &input_map,
    std::unordered_map<int, std::vector<struct input_event>> &frames,
    int epfd
);

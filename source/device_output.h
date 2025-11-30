#pragma once

#include <string>

#include <libevdev/libevdev-uinput.h>

struct OutputDecl {
  std::string id;
  std::string type;
  std::string name;
};

libevdev_uinput *create_output_device(const OutputDecl &out);

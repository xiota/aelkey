#pragma once

#include <string>
#include <vector>

struct InputDecl {
  std::string id;
  std::string type;
  int vendor = 0;
  int product = 0;
  int bus = 0;
  int interface = -1;
  std::string name;
  std::string phys;
  std::string uniq;

  bool grab = false;
  std::vector<std::pair<int, int>> capabilities;

  int service = 0;
  int characteristic = 0;

  std::string devnode;

  std::string on_event;  // HID input events
  std::string on_state;  // lifecycle events

  // jack midi/audio
  std::string client;
  std::string port;

  int fd = -1;
};

struct OutputDecl {
  std::string id;
  std::string type;
  int vendor = 0x1234;
  int product = 0x5678;
  int bus = 3;
  int version = 1;
  std::string name;
  std::string on_haptics;
  std::vector<std::string> capabilities;
};

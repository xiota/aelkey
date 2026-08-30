#pragma once

#include <string>
#include <vector>

struct InputDecl {
  std::string id;
  std::string type;

  int vendor = 0;
  int product = 0;
  std::vector<std::pair<int, int>> vid_pid;

  int bus = 0;
  std::vector<int> interfaces;
  std::string name;
  std::string phys;
  std::string uniq;

  bool grab = false;
  std::vector<std::pair<int, int>> capabilities;

  std::vector<int> services;
  std::vector<int> characteristics;

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
  std::string profile;
  int vendor = 0x1234;
  int product = 0x5678;
  int bus = 3;
  int version = 1;
  std::string name;
  std::string on_haptics;
  std::vector<std::string> capabilities;

  // jack midi/audio
  std::string client;
  std::string port;

  // uhid
  std::string on_report;
  std::string report_desc;
  std::string phys;
  std::string uniq;
  int country = 0;
};

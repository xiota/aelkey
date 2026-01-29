#pragma once

#include <map>
#include <string>
#include <vector>

#include <libevdev/libevdev-uinput.h>
#include <linux/input.h>
#include <sol/sol.hpp>

#include "device_declarations.h"
#include "singleton.h"

class AelkeyState : public Singleton<AelkeyState> {
  friend class Singleton<AelkeyState>;

 private:
  AelkeyState() = default;
  ~AelkeyState() = default;

  bool on_init() override;

  bool auto_init_ = true;

 public:
  // Attach all input devices declared in input_decls
  void attach_inputs_from_decls(sol::this_state ts);

  // Create all uinput output devices declared in output_decls
  void create_outputs_from_decls();

  // Parse global "inputs" table from the given Lua state
  void parse_inputs_from_lua(sol::this_state ts);

  // Parse global "outputs" table from the given Lua state
  void parse_outputs_from_lua(sol::this_state ts);

 public:
  lua_State *lua_vm = nullptr;

  int epfd = -1;
  std::map<std::string, libevdev_uinput *> uinput_devices;
  std::map<std::string, InputDecl> input_map;
  std::map<std::string, std::vector<struct input_event>> frames;

  bool loop_should_stop = false;
  int sigint = 0;

  std::vector<InputDecl> input_decls;
  std::vector<OutputDecl> output_decls;

  std::map<std::string, std::vector<InputDecl>> watch_map;

  std::string on_watchlist;
};

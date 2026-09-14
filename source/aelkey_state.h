#pragma once

#include <atomic>
#include <map>
#include <string>
#include <vector>

#include <linux/input.h>
#include <sol/sol.hpp>

#include "device_declarations.h"
#include "singleton.h"
#include "utils/signal.h"

class AelkeyState : public Singleton<AelkeyState> {
  friend class Singleton<AelkeyState>;

 private:
  AelkeyState() = default;
  ~AelkeyState() = default;

  bool on_init() override;

  bool auto_init_ = true;

 public:
  // Attach all input devices declared in input_decls
  void attach_inputs_from_decls();

  // Create all uinput output devices declared in output_decls
  void create_outputs_from_decls();

  // Parse global "inputs" table from the given Lua state
  void parse_inputs_from_lua(sol::this_state ts);

  // Parse global "outputs" table from the given Lua state
  void parse_outputs_from_lua(sol::this_state ts);

  // Task counter methods for safe loop shutdown tracking
  void increment_active_tasks() {
    active_tasks_.fetch_add(1, std::memory_order_relaxed);
  }

  void decrement_active_tasks() {
    active_tasks_.fetch_sub(1, std::memory_order_relaxed);
  }

  bool is_safe_to_stop() const {
    return active_tasks_.load(std::memory_order_relaxed) <= 0;
  }

  // epoll cycle signal
  auto subscribe_epoll_cycle(AelkeyUtil::Signal<void(void)>::Callback cb) {
    return sig_epoll_cycle_.subscribe(std::move(cb));
  }

  void notify_epoll_cycle() {
    sig_epoll_cycle_.emit();
  }

  // shutdown signal
  auto subscribe_shutdown(AelkeyUtil::Signal<void(void)>::Callback cb) {
    return sig_shutdown_.subscribe(std::move(cb));
  }

  void notify_shutdown() {
    sig_shutdown_.emit();
  }

 public:
  lua_State *lua_vm = nullptr;

  int epfd = -1;
  std::map<std::string, InputDecl> input_map;

  bool loop_running = false;
  bool loop_should_stop = false;

  std::atomic<int> active_tasks_{ 0 };

  std::vector<InputDecl> input_decls;
  std::vector<OutputDecl> output_decls;

 private:
  AelkeyUtil::Signal<void(void)> sig_epoll_cycle_;
  AelkeyUtil::Signal<void(void)> sig_shutdown_;
};

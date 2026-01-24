#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>

#include <sol/sol.hpp>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include "aelkey_state.h"
#include "dispatcher.h"
#include "dispatcher_registry.h"

struct TickCb {
  bool is_function = false;      // true if using a sol::function
  sol::function fn;              // Lua callback (if is_function == true)
  std::string name;              // Lua global name (if not using fn)
  std::function<void()> native;  // native C++ callback
  bool oneshot = false;          // if true, timer is removed after first fire
};

class TickScheduler : public Dispatcher<TickScheduler> {
 public:
  TickScheduler() = default;
  ~TickScheduler() {
    cancel_all();
  }

  void unregister_fd(int fd) {
    DispatcherBase::unregister_fd(fd);
    close(fd);
  }

  void handle_event(EpollPayload *payload, uint32_t /*events*/) override {
    int fd = payload->fd;
    uint64_t expirations;
    if (read(fd, &expirations, sizeof(expirations)) < 0) {
      return;  // EAGAIN or transient error
    }

    auto it = callbacks_.find(fd);
    if (it == callbacks_.end()) {
      return;
    }

    auto cb = it->second;  // copy so we can erase safely after

    if (cb.native) {
      try {
        cb.native();
      } catch (const std::exception &e) {
        fprintf(stderr, "tick native error: %s\n", e.what());
      } catch (...) {
        fprintf(stderr, "tick native error: unknown exception\n");
      }
    } else if (cb.is_function && cb.fn.valid()) {
      sol::protected_function pf = cb.fn;
      sol::protected_function_result result = pf();
      if (!result.valid()) {
        sol::error err = result;
        fprintf(stderr, "tick function error: %s\n", err.what());
      }
    } else if (!cb.name.empty()) {
      sol::state_view lua_state(AelkeyState::instance().lua_vm);
      sol::object obj = lua_state[cb.name];
      if (obj.is<sol::function>()) {
        sol::protected_function pf = obj.as<sol::function>();
        sol::protected_function_result result = pf();
        if (!result.valid()) {
          sol::error err = result;
        }
      }
    }

    if (cb.oneshot) {
      unregister_fd(fd);
    }
  }

  // Schedule a timer with the given callback.
  // - ms: delay/interval in milliseconds
  // - cb: callback descriptor (Lua function, global name, or native)
  // Returns timerfd on success, -1 on failure.
  int schedule(int ms, TickCb cb) {
    int fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
    if (fd < 0) {
      perror("timerfd_create");
      return -1;
    }

    struct itimerspec spec{};
    spec.it_value.tv_sec = ms / 1000;
    spec.it_value.tv_nsec = (ms % 1000) * 1000000;

    if (cb.oneshot) {
      spec.it_interval.tv_sec = 0;
      spec.it_interval.tv_nsec = 0;
    } else {
      spec.it_interval = spec.it_value;
    }

    if (timerfd_settime(fd, 0, &spec, nullptr) < 0) {
      perror("timerfd_settime");
      close(fd);
      return -1;
    }

    register_fd(fd, EPOLLIN);
    callbacks_[fd] = std::move(cb);
    return fd;
  }

  void cancel_matching(const TickCb &key) {
    for (auto it = callbacks_.begin(); it != callbacks_.end();) {
      auto &existing = it->second;
      bool match = false;

      if (key.is_function && existing.is_function) {
        match = (existing.fn == key.fn);
      } else if (!key.is_function && !existing.is_function) {
        match = (existing.name == key.name);
      }

      if (match) {
        int fd = it->first;
        unregister_fd(fd);
        it = callbacks_.erase(it);
      } else {
        ++it;
      }
    }
  }

  // Cancel any timers whose callback matches the provided key.
  // Matching rules:
  // - if key.is_function && existing.is_function: compare sol::function identity
  // - if both are name-based: compare name strings
  void cancel_all() {
    for (auto &[fd, cb] : callbacks_) {
      unregister_fd(fd);
    }
    callbacks_.clear();
  }

 private:
  std::unordered_map<int, TickCb> callbacks_;
};

template class Dispatcher<TickScheduler>;

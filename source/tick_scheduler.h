#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>

#include <sol/sol.hpp>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include "aelkey_state.h"
#include "dispatcher.h"
#include "dispatcher_next.h"

class TickScheduler : public DispatcherNext<TickScheduler> {
  friend class Singleton<TickScheduler>;

 protected:
  TickScheduler() = default;
  ~TickScheduler() {
    cancel_all();
    for (int i = 0; i < 3; ++i) {
      flush_deferred();
    }
  }

 public:
  const char *type() const override {
    return "tick";
  }

  void on_unregister(int fd) override {
    clear_callback(fd);
    close(fd);
  }

  // Pre-callback: drain timerfd
  bool on_handle_event_before(int fd) override {
    uint64_t expirations;
    if (read(fd, &expirations, sizeof(expirations)) < 0) {
      // EAGAIN or transient error: skip callback
      return false;
    }
    return true;
  }

  // Schedule a timer with the given callback.
  // - ms: delay/interval in milliseconds
  // - cb: callback descriptor (Lua function, global name, or native)
  // Returns timerfd on success, -1 on failure.
  int schedule(int ms, DispatcherCb cb) {
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
    set_callback(fd, std::move(cb));
    return fd;
  }

  // Expire timerfd and preserve current recurring interval.
  // Safe to call from device_in background threads.
  void trigger(int fd) {
    if (fd < 0) {
      return;
    }

    struct itimerspec old_spec{};
    if (timerfd_gettime(fd, &old_spec) < 0) {
      return;
    }

    struct itimerspec spec{};
    spec.it_value.tv_sec = 0;
    spec.it_value.tv_nsec = 1;
    spec.it_interval = old_spec.it_interval;

    if (timerfd_settime(fd, 0, &spec, nullptr) < 0) {
      perror("timerfd_settime");
    }
  }

  void cancel_matching(const DispatcherCb &key) {
    // iterate over callbacks_ in base
    for (auto it = callbacks_.begin(); it != callbacks_.end(); ++it) {
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
      }
    }
  }

  void cancel_all() {
    for (auto &[fd, cb] : callbacks_) {
      unregister_fd(fd);
    }
    callbacks_.clear();
  }

 private:
  using DispatcherNextBase::callbacks_;
};

template class DispatcherNext<TickScheduler>;

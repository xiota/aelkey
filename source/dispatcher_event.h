#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>

#include <sol/sol.hpp>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include "dispatcher.h"
#include "singleton.h"

class DispatcherEvent : public Dispatcher<DispatcherEvent> {
  friend class Singleton<DispatcherEvent>;

 protected:
  DispatcherEvent() = default;
  ~DispatcherEvent() {
    cancel_all();
    for (int i = 0; i < 3; ++i) {
      flush_deferred();
    }
  }

 public:
  const char *type() const override {
    return "event";
  }

  void on_unregister(int fd) override {
    clear_callback(fd);
    close(fd);
  }

  // Drain eventfd before callback
  bool on_handle_event_before(int fd) override {
    uint64_t value;
    if (read(fd, &value, sizeof(value)) < 0) {
      return false;
    }
    return true;
  }

  // Create an eventfd and register it
  int create(DispatcherCb cb) {
    int fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (fd < 0) {
      perror("eventfd");
      return -1;
    }

    register_fd(fd, EPOLLIN);
    set_callback(fd, std::move(cb));
    return fd;
  }

  // Trigger an eventfd from any thread
  void trigger(int fd) {
    if (fd < 0) {
      return;
    }

    uint64_t inc = 1;
    if (write(fd, &inc, sizeof(inc)) < 0) {
      // EAGAIN is fine; eventfd accumulates
    }
  }

  void cancel_matching(const DispatcherCb &key) {
    for (auto it = callbacks_.begin(); it != callbacks_.end(); ++it) {
      auto &existing = it->second;
      bool match = false;

      if (key.is_function && existing.is_function) {
        match = (existing.fn == key.fn);
      } else if (!key.is_function && !existing.is_function) {
        match = (existing.name == key.name);
      }

      if (match) {
        unregister_fd(it->first);
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
  using DispatcherBase::callbacks_;
};

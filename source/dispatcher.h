#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include <sol/sol.hpp>

#include "singleton.h"

class AelkeyState;
class EpollPayload;

struct DispatcherCb {
  bool is_function = false;      // true if using a sol::function
  sol::function fn;              // Lua callback (if is_function == true)
  std::string name;              // Lua global name (if not using fn)
  std::function<void()> native;  // native C++ callback
  bool oneshot = false;          // if true, fd is removed after first fire

  std::function<void(int fd)> cleanup;  // cleanup hook
};

// Polymorphic base class for all next-gen dispatchers
class DispatcherBase {
 public:
  virtual bool lazy_init() = 0;
  virtual const char *type() const = 0;

  // Unified event handler
  virtual void handle_event(EpollPayload *payload, uint32_t events);

  EpollPayload *get_payload(int fd) const;

  virtual void register_fd(int fd, uint32_t events);
  virtual void unregister_fd(int fd);
  virtual void cleanup_fds();
  virtual void flush_deferred();

  // This function is called when it is safe to close.
  // Closing earlier will cause intermittent segfaults.
  virtual void on_unregister(int fd) {
    auto it = callbacks_.find(fd);
    if (it != callbacks_.end() && it->second.cleanup) {
      it->second.cleanup(fd);
    }
  }

  // Hooks around generic callback handling
  // Return false from before() to skip callback.
  virtual bool on_handle_event_before(int fd) {
    return true;
  }
  virtual void on_handle_event_after(int fd) {}

 protected:
  DispatcherBase() = default;

  // Callback management
  void set_callback(int fd, DispatcherCb cb) {
    callbacks_[fd] = std::move(cb);
  }

  DispatcherCb *get_callback(int fd) {
    auto it = callbacks_.find(fd);
    return (it != callbacks_.end()) ? &it->second : nullptr;
  }

  void clear_callback(int fd) {
    callbacks_.erase(fd);
  }

  std::map<int, EpollPayload> pollfds_;
  std::map<int, DispatcherCb> callbacks_;

  int cycle_ = 0;
  std::vector<int> deferred_unregs_[3];
};

// CRTP dispatcher class
template <typename Derived>
class Dispatcher : public DispatcherBase, public Singleton<Derived> {
  friend class Singleton<Derived>;

 public:
  bool lazy_init() override {
    return Singleton<Derived>::lazy_init();
  }

 protected:
  Dispatcher() = default;
};

struct EpollPayload {
  DispatcherBase *dispatcher = nullptr;
  int fd = -1;
  bool dead = false;

  void dispatch(uint32_t events) {
    dispatcher->handle_event(this, events);
  }
};

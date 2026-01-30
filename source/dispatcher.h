#pragma once

#include <cstdint>
#include <map>
#include <sys/epoll.h>
#include <vector>

#include "dispatcher_registry.h"
#include "singleton.h"

class AelkeyState;
class DispatcherBase;

struct EpollPayload {
  DispatcherBase *dispatcher = nullptr;
  int fd = -1;
  bool dead = false;
};

// Polymorphic base class for all dispatchers
class DispatcherBase {
 public:
  virtual ~DispatcherBase();

  virtual bool lazy_init() = 0;
  virtual void handle_event(EpollPayload *payload, uint32_t events) {}
  virtual const char *type() const = 0;

  EpollPayload *get_payload(int fd) const;

  virtual void register_fd(int fd, uint32_t events);
  virtual void on_unregister(int fd) {}
  virtual void unregister_fd(int fd);
  virtual void cleanup_fds();
  virtual void flush_deferred();

 protected:
  DispatcherBase() = default;

  std::map<int, EpollPayload> pollfds_;
  int cycle_ = 0;
  std::vector<int> deferred_unregs_[2];
};

// CRTP dispatcher class
template <typename Derived>
class Dispatcher : public DispatcherBase, public Singleton<Derived> {
  friend class Singleton<Derived>;

 public:
  bool lazy_init() override {
    return Singleton<Derived>::lazy_init();
  }

  static void register_self() {
    auto &reg = dispatcher_registry();
    reg[Derived::instance().type()] = &Derived::instance();
  }

 protected:
  Dispatcher() = default;
};

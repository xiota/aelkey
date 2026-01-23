#pragma once

#include <cstdint>
#include <vector>

template <typename T>
struct DispatcherRegistry;

class DispatcherBase;

struct EpollPayload {
  DispatcherBase *dispatcher;
  int fd;
};

// Polymorphic base class for all dispatchers
class DispatcherBase {
 public:
  virtual ~DispatcherBase() = default;

  virtual void init() {}
  virtual void handle_event(EpollPayload *payload, uint32_t events) {};

 protected:
  DispatcherBase() = default;
};

// CRTP dispatcher class
template <typename Derived>
class Dispatcher : public DispatcherBase {
 public:
  static Derived &instance() {
    static Derived inst;
    return inst;
  }

  Dispatcher(const Dispatcher &) = delete;
  Dispatcher &operator=(const Dispatcher &) = delete;
  Dispatcher(Dispatcher &&) = delete;
  Dispatcher &operator=(Dispatcher &&) = delete;

 protected:
  Dispatcher() = default;

 private:
  static DispatcherRegistry<Derived> reg_;
};

// Static member definition
template <typename Derived>
DispatcherRegistry<Derived> Dispatcher<Derived>::reg_{};

#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include <sys/epoll.h>

#include "aelkey_state.h"

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
  virtual ~DispatcherBase() {
    cleanup_fds();
  }

  virtual void init() {}
  virtual void handle_event(EpollPayload *payload, uint32_t events) {};

  EpollPayload *get_payload(int fd) const {
    auto it = pollfds_.find(fd);
    return (it != pollfds_.end()) ? it->second : nullptr;
  }

  virtual void register_fd(int fd, uint32_t events) {
    auto &state = AelkeyState::instance();

    auto *payload = new EpollPayload{ this, fd };

    struct epoll_event ev{};
    ev.events = events;
    ev.data.ptr = payload;

    if (epoll_ctl(state.epfd, EPOLL_CTL_ADD, fd, &ev) < 0) {
      perror("epoll_ctl ADD");
      delete payload;
      return;
    }

    pollfds_[fd] = payload;
  }

  virtual void unregister_fd(int fd) {
    auto &state = AelkeyState::instance();

    auto it = pollfds_.find(fd);
    if (it == pollfds_.end()) {
      return;
    }

    epoll_ctl(state.epfd, EPOLL_CTL_DEL, fd, nullptr);

    delete it->second;
    pollfds_.erase(it);
  }

  virtual void cleanup_fds() {
    auto &state = AelkeyState::instance();

    for (auto &[fd, payload] : pollfds_) {
      epoll_ctl(state.epfd, EPOLL_CTL_DEL, fd, nullptr);
      delete payload;
    }
    pollfds_.clear();
  }

 protected:
  DispatcherBase() = default;
  std::unordered_map<int, EpollPayload *> pollfds_;
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

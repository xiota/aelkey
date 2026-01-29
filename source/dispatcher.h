#pragma once

#include <cstdint>
#include <map>
#include <vector>

#include <sys/epoll.h>

#include "aelkey_state.h"
#include "singleton.h"

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

  virtual const char *type() const = 0;

  EpollPayload *get_payload(int fd) const {
    auto it = pollfds_.find(fd);
    return (it != pollfds_.end()) ? const_cast<EpollPayload *>(&it->second) : nullptr;
  }

  virtual void register_fd(int fd, uint32_t events) {
    auto &state = AelkeyState::instance();

    // Insert payload by value
    EpollPayload payload{ this, fd };
    auto [it, inserted] = pollfds_.emplace(fd, payload);

    struct epoll_event ev{};
    ev.events = events;
    ev.data.ptr = &it->second;  // stable pointer (std::map node)

    if (epoll_ctl(state.epfd, EPOLL_CTL_ADD, fd, &ev) < 0) {
      perror("epoll_ctl ADD");
      pollfds_.erase(it);
      return;
    }
  }

  virtual void unregister_fd(int fd) {
    auto &state = AelkeyState::instance();

    auto it = pollfds_.find(fd);
    if (it == pollfds_.end()) {
      return;
    }

    epoll_ctl(state.epfd, EPOLL_CTL_DEL, fd, nullptr);

    // Erase destroys the EpollPayload immediately
    pollfds_.erase(it);
  }

  virtual void cleanup_fds() {
    auto &state = AelkeyState::instance();

    for (auto &[fd, payload] : pollfds_) {
      epoll_ctl(state.epfd, EPOLL_CTL_DEL, fd, nullptr);
      // payload is destroyed automatically when map is cleared
    }
    pollfds_.clear();
  }

 protected:
  DispatcherBase() = default;

  // std::map ensures stable node addresses
  std::map<int, EpollPayload> pollfds_;
};

// CRTP dispatcher class
template <typename Derived>
class Dispatcher : public DispatcherBase, public Singleton<Derived> {
  friend class Singleton<Derived>;

 protected:
  Dispatcher() = default;

 private:
  static DispatcherRegistry<Derived> reg_;
};

// Static member definition
template <typename Derived>
DispatcherRegistry<Derived> Dispatcher<Derived>::reg_{};

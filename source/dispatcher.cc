#include "dispatcher.h"

#include <cstdio>

#include <sys/epoll.h>

#include "aelkey_state.h"

DispatcherBase::~DispatcherBase() {
  cleanup_fds();
}

EpollPayload *DispatcherBase::get_payload(int fd) const {
  auto it = pollfds_.find(fd);
  return (it != pollfds_.end()) ? const_cast<EpollPayload *>(&it->second) : nullptr;
}

void DispatcherBase::register_fd(int fd, uint32_t events) {
  auto &state = AelkeyState::instance();

  EpollPayload payload{ this, fd };
  auto [it, inserted] = pollfds_.emplace(fd, payload);

  struct epoll_event ev{};
  ev.events = events;
  ev.data.ptr = &it->second;

  if (epoll_ctl(state.epfd, EPOLL_CTL_ADD, fd, &ev) < 0) {
    perror("epoll_ctl ADD");
    pollfds_.erase(it);
    return;
  }
}

void DispatcherBase::unregister_fd(int fd) {
  auto &state = AelkeyState::instance();

  epoll_ctl(state.epfd, EPOLL_CTL_DEL, fd, nullptr);

  auto it = pollfds_.find(fd);
  if (it != pollfds_.end()) {
    it->second.dead = true;
    deferred_unregs_[cycle_].push_back(fd);
  }
}

void DispatcherBase::cleanup_fds() {
  auto &state = AelkeyState::instance();

  for (auto &[fd, payload] : pollfds_) {
    epoll_ctl(state.epfd, EPOLL_CTL_DEL, fd, nullptr);
    on_unregister(fd);
  }
  pollfds_.clear();
}

void DispatcherBase::flush_deferred() {
  // cycle: writing in current cycle, unsafe to flush
  // cycle - 1 (+2 % 3): wrote last cycle, safe to flush
  int prev = (cycle_ + 2) % 3;
  auto &list = deferred_unregs_[prev];

  if (!list.empty()) {
    for (int fd : list) {
      auto it = pollfds_.find(fd);
      if (it != pollfds_.end()) {
        on_unregister(fd);
        pollfds_.erase(it);
      }
    }
    list.clear();
  }

  // cycle + 1: already cleared, ready for next cycle
  cycle_ = (cycle_ + 1) % 3;
}

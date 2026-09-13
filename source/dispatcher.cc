#include "dispatcher.h"

#include <cstdio>

#include <sys/epoll.h>

#include "aelkey_state.h"

EpollPayload *DispatcherBase::get_payload(int fd) const {
  auto it = pollfds_.find(fd);
  return (it != pollfds_.end()) ? const_cast<EpollPayload *>(&it->second) : nullptr;
}

void DispatcherBase::register_fd(int fd, uint32_t events) {
  EpollPayload payload;
  payload.dispatcher = this;
  payload.fd = fd;

  auto [it, inserted] = pollfds_.emplace(fd, payload);

  struct epoll_event ev{};
  ev.events = events;
  ev.data.ptr = &it->second;

  auto &state = AelkeyState::instance();
  if (state.epfd >= 0) {
    if (epoll_ctl(state.epfd, EPOLL_CTL_ADD, fd, &ev) < 0) {
      perror("epoll_ctl ADD");
      pollfds_.erase(it);
      return;
    }
  }
}

void DispatcherBase::unregister_fd(int fd) {
  auto &state = AelkeyState::instance();

  if (state.epfd >= 0) {
    epoll_ctl(state.epfd, EPOLL_CTL_DEL, fd, nullptr);
  }

  auto it = pollfds_.find(fd);
  if (it != pollfds_.end()) {
    it->second.dead = true;
    deferred_unregs_[cycle_].push_back(fd);
  }
}

void DispatcherBase::cleanup_fds() {
  auto &state = AelkeyState::instance();

  for (auto &[fd, payload] : pollfds_) {
    if (state.epfd >= 0) {
      epoll_ctl(state.epfd, EPOLL_CTL_DEL, fd, nullptr);
    }
    on_unregister(fd);
  }
  pollfds_.clear();
  callbacks_.clear();
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
        clear_callback(fd);
      }
    }
    list.clear();
  }

  // cycle + 1: already cleared, ready for next cycle
  cycle_ = (cycle_ + 1) % 3;
}

void DispatcherBase::handle_event(EpollPayload *payload, uint32_t /*events*/) {
  if (!payload) {
    return;
  }

  int fd = payload->fd;

  if (!on_handle_event_before(fd)) {
    return;
  }

  DispatcherCb *cb = get_callback(fd);
  if (!cb) {
    return;
  }

  // Generic callback handling
  if (cb->native) {
    try {
      cb->native();
    } catch (const std::exception &e) {
      std::fprintf(stderr, "%s native error: %s\n", type(), e.what());
    } catch (...) {
      std::fprintf(stderr, "%s native error: unknown exception\n", type());
    }
  } else if (cb->is_function && cb->fn.valid()) {
    sol::protected_function pf = cb->fn;
    sol::protected_function_result result = pf();
    if (!result.valid()) {
      sol::error err = result;
      std::fprintf(stderr, "%s function error: %s\n", type(), err.what());
    }
  } else if (!cb->name.empty()) {
    sol::state_view lua_state(AelkeyState::instance().lua_vm);
    sol::object obj = lua_state[cb->name];
    if (obj.is<sol::function>()) {
      sol::protected_function pf = obj.as<sol::function>();
      sol::protected_function_result result = pf();
      if (!result.valid()) {
        sol::error err = result;
        std::fprintf(stderr, "%s name function error: %s\n", type(), err.what());
      }
    }
  }

  on_handle_event_after(fd);

  if (cb->oneshot) {
    unregister_fd(fd);
  }
}

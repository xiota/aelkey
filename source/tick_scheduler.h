#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>

#include <errno.h>
#include <lua.hpp>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <unistd.h>

struct TickCb {
  bool is_function = false;      // Lua function flag
  int ref = LUA_NOREF;           // Lua registry reference
  std::string name;              // Lua global name
  std::function<void()> native;  // native C++ callback
  bool oneshot = false;
};

class TickScheduler {
 public:
  TickScheduler(int epfd, lua_State *L) : epfd_(epfd), L_(L) {}
  ~TickScheduler() {
    cancel_all();
  }

  bool owns_fd(int fd) const {
    return callbacks_.find(fd) != callbacks_.end();
  }

  // Feature parity: schedule repeating timer by string name or Lua function ref
  // Returns fd on success, -1 on failure
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
      spec.it_interval = spec.it_value;  // repeat with the same interval
    }

    if (timerfd_settime(fd, 0, &spec, nullptr) < 0) {
      perror("timerfd_settime");
      close(fd);
      return -1;
    }

    struct epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = fd;
    if (epoll_ctl(epfd_, EPOLL_CTL_ADD, fd, &ev) < 0) {
      perror("epoll_ctl EPOLL_CTL_ADD (tick)");
      close(fd);
      return -1;
    }

    callbacks_[fd] = std::move(cb);
    return fd;
  }

  // Cancel a specific callback that matches the provided key
  // Matches by string name OR by Lua function identity (rawequal)
  void cancel_matching(const TickCb &key) {
    for (auto it = callbacks_.begin(); it != callbacks_.end();) {
      auto &existing = it->second;
      bool match = false;

      if (key.is_function && existing.is_function) {
        // Compare functions by identity
        lua_rawgeti(L_, LUA_REGISTRYINDEX, existing.ref);
        lua_rawgeti(L_, LUA_REGISTRYINDEX, key.ref);
        match = lua_rawequal(L_, -1, -2);
        lua_pop(L_, 2);
      } else if (!key.is_function && !existing.is_function) {
        match = (existing.name == key.name);
      }

      if (match) {
        int fd = it->first;
        epoll_ctl(epfd_, EPOLL_CTL_DEL, fd, nullptr);
        close(fd);
        if (existing.is_function && existing.ref != LUA_NOREF) {
          luaL_unref(L_, LUA_REGISTRYINDEX, existing.ref);
        }
        it = callbacks_.erase(it);
      } else {
        ++it;
      }
    }
  }

  // Cancel all timers
  void cancel_all() {
    for (auto &[fd, cb] : callbacks_) {
      epoll_ctl(epfd_, EPOLL_CTL_DEL, fd, nullptr);
      close(fd);
      if (cb.is_function && cb.ref != LUA_NOREF) {
        luaL_unref(L_, LUA_REGISTRYINDEX, cb.ref);
      }
    }
    callbacks_.clear();
  }

  // Handle a readable timerfd from epoll loop; call Lua callback by name or function
  void handle_event(int fd) {
    uint64_t expirations;
    if (read(fd, &expirations, sizeof(expirations)) < 0) {
      // Non-fatal; if EAGAIN, just return
      return;
    }

    auto it = callbacks_.find(fd);
    if (it == callbacks_.end()) {
      return;
    }

    auto &cb = it->second;

    if (cb.native) {
      // Call native C++ function
      try {
        cb.native();
      } catch (const std::exception &e) {
        fprintf(stderr, "tick native error: %s\n", e.what());
      }
    } else if (cb.is_function) {
      lua_rawgeti(L_, LUA_REGISTRYINDEX, cb.ref);
      // pcall with 0 args, 0 results, 0 error func
      if (lua_pcall(L_, 0, 0, 0) != LUA_OK) {
        const char *err = lua_tostring(L_, -1);
        fprintf(stderr, "tick function error: %s\n", err ? err : "(unknown)");
        lua_pop(L_, 1);
      }
    } else {
      // Call global function by name if it exists
      lua_getglobal(L_, cb.name.c_str());
      if (lua_isfunction(L_, -1)) {
        if (lua_pcall(L_, 0, 0, 0) != LUA_OK) {
          const char *err = lua_tostring(L_, -1);
          fprintf(stderr, "tick '%s' error: %s\n", cb.name.c_str(), err ? err : "(unknown)");
          lua_pop(L_, 1);
        }
      } else {
        lua_pop(L_, 1);  // not a function; silently ignore
      }
    }

    if (cb.oneshot) {
      epoll_ctl(epfd_, EPOLL_CTL_DEL, fd, nullptr);
      close(fd);
      if (cb.is_function && cb.ref != LUA_NOREF) {
        luaL_unref(L_, LUA_REGISTRYINDEX, cb.ref);
      }
      callbacks_.erase(it);
    }
  }

 private:
  int epfd_;
  lua_State *L_;
  std::unordered_map<int, TickCb> callbacks_;
};

#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include <fcntl.h>
#include <sol/sol.hpp>
#include <unistd.h>

#include "aelkey_state.h"
#include "device_helpers.h"  // for match_string, etc.
#include "device_input.h"    // for InputDecl
#include "dispatcher.h"
#include "dispatcher_registry.h"

class DispatcherHidraw : public Dispatcher<DispatcherHidraw> {
  friend class Singleton<DispatcherHidraw>;

 protected:
  DispatcherHidraw() = default;
  ~DispatcherHidraw() = default;

 public:
  const char *type() const override {
    return "hidraw";
  }

  void unregister_fd(int fd) {
    DispatcherBase::unregister_fd(fd);
    close(fd);
  }

  int open_device(const std::string &devnode, const InputDecl &decl) {
    int fd = ::open(devnode.c_str(), O_RDWR | O_NONBLOCK);
    if (fd < 0) {
      perror("open hidraw");
      return -1;
    }

    if (decl.grab) {
      int flags = fcntl(fd, F_GETFL, 0);
      if (flags != -1) {
        fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
      }
    }

    // Register with dispatcher (creates EpollPayload)
    register_fd(fd, EPOLLIN);

    // Store InputDecl by FD
    devices_[fd] = decl;

    return fd;
  }

  void remove_device(const std::string &id) {
    for (auto it = devices_.begin(); it != devices_.end(); ++it) {
      if (it->second.id == id) {
        int fd = it->first;
        unregister_fd(fd);
        devices_.erase(it);
        return;
      }
    }
  }

  // Called by epoll loop
  void handle_event(EpollPayload *payload, uint32_t events) override {
    int fd = payload->fd;

    auto it = devices_.find(fd);
    if (it == devices_.end()) {
      return;
    }

    handle_hidraw_event(fd, it->second, events);
  }

 private:
  void handle_hidraw_event(int fd, const InputDecl &decl, uint32_t events) {
    if (!(events & EPOLLIN)) {
      return;
    }

    uint8_t buf[4096];
    ssize_t r = ::read(fd, buf, sizeof(buf));

    if (decl.on_event.empty()) {
      return;
    }

    auto &state = AelkeyState::instance();
    sol::state_view lua(state.lua_vm);

    sol::object obj = lua[decl.on_event];
    if (!obj.is<sol::function>()) {
      return;
    }

    sol::function cb = obj.as<sol::function>();

    sol::table tbl = lua.create_table();
    tbl["device"] = decl.id;

    if (r > 0) {
      tbl["data"] = std::string_view(reinterpret_cast<const char *>(buf), r);
      tbl["size"] = static_cast<int>(r);
      tbl["status"] = "ok";
    } else if (r == 0) {
      tbl["status"] = "disconnect";
    } else {
      tbl["status"] = strerror(errno);
    }

    sol::protected_function pf = cb;
    sol::protected_function_result res = pf(tbl);
    if (!res.valid()) {
      sol::error err = res;
      fprintf(stderr, "Lua hidraw callback error: %s\n", err.what());
    }
  }

  std::unordered_map<int, InputDecl> devices_;
};

template class Dispatcher<DispatcherHidraw>;

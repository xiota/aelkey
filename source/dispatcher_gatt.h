#pragma once

#include <iostream>
#include <map>
#include <sys/epoll.h>
#include <unistd.h>

#include "aelkey_state.h"
#include "device_declarations.h"
#include "dispatcher.h"
#include "manager_device_in.h"
#include "singleton.h"

struct GattDeviceState {
  std::string id;    // stable device identifier
  std::string path;  // characteristic object path
  int fd = -1;       // acquired notification file descriptor
  uint16_t mtu = 0;  // negotiated MTU
};

class DispatcherGatt : public Dispatcher<DispatcherGatt> {
  friend class Singleton<DispatcherGatt>;
  friend class Dispatcher<DispatcherGatt>;

 protected:
  DispatcherGatt() = default;
  ~DispatcherGatt() = default;

 public:
  const char *type() const override {
    return "gatt";
  }

  void on_unregister(int fd) override {
    close(fd);
  }

  bool open_device_notify(const std::string &devnode, InputDecl &decl, int fd, uint16_t mtu) {
    if (fd < 0) {
      return false;
    }

    register_fd(fd, EPOLLIN | EPOLLHUP | EPOLLERR);

    GattDeviceState st;
    st.id = decl.id;
    st.path = devnode;
    st.fd = fd;
    st.mtu = mtu;
    devs_[fd] = std::move(st);

    return true;
  }

  void close_device(InputDecl &decl) {
    for (auto it = devs_.begin(); it != devs_.end(); ++it) {
      if (it->second.id == decl.id) {
        unregister_fd(it->first);
        devs_.erase(it);
        break;
      }
    }
  }

  void handle_event(EpollPayload *payload, uint32_t events) override {
    int fd = payload->fd;

    auto it = devs_.find(fd);
    if (it == devs_.end()) {
      return;
    }
    auto &st = it->second;

    if (events & (EPOLLHUP | EPOLLERR)) {
      ManagerDeviceIn::instance().detach(st.id);
      return;
    }

    if (!(events & EPOLLIN)) {
      return;
    }

    // Read raw ATT packet directly from socket
    std::vector<uint8_t> buffer(st.mtu > 0 ? st.mtu : 512);
    ssize_t n = read(fd, buffer.data(), buffer.size());
    if (n <= 0) {
      return;
    }
    buffer.resize(n);

    dispatch_gatt_event(st.id, st.path, buffer);
  }

 private:
  void dispatch_gatt_event(
      const std::string &id,
      const std::string &path,
      const std::vector<uint8_t> &data
  ) {
    auto &state = AelkeyState::instance();
    auto decl_it = state.input_map.find(id);
    if (decl_it == state.input_map.end()) {
      return;
    }
    InputDecl &decl = decl_it->second;

    if (decl.on_event.empty()) {
      return;
    }

    sol::state_view lua(state.lua_vm);
    sol::object obj = lua[decl.on_event];
    if (!obj.is<sol::function>()) {
      return;
    }

    sol::function cb = obj.as<sol::function>();
    sol::table tbl = lua.create_table();
    tbl["device"] = decl.id;
    tbl["path"] = path;
    tbl["data"] = std::string_view(reinterpret_cast<const char *>(data.data()), data.size());
    tbl["size"] = static_cast<int>(data.size());
    tbl["status"] = "ok";

    sol::protected_function pf = cb;
    sol::protected_function_result res = pf(tbl);
    if (!res.valid()) {
      sol::error err = res;
      std::fprintf(stderr, "Lua gatt_callback error: %s\n", err.what());
    }
  }

  std::map<int, GattDeviceState> devs_;
};

template class Dispatcher<DispatcherGatt>;

#pragma once

#include "device_in_gatt.h"
#include "dispatcher.h"

class DispatcherGATT : public Dispatcher<DispatcherGATT> {
  friend class Singleton<DispatcherGATT>;
  friend class Dispatcher<DispatcherGATT>;

 public:
  const char *type() const override {
    return "gatt";
  }

  void handle_event(EpollPayload *, uint32_t events) override {
    if (events & EPOLLIN) {
      DeviceInGatt::instance().pump_messages();
    }
  }

 protected:
  bool on_init() override {
    int fd = DeviceInGatt::instance().fd();
    if (fd < 0) {
      return false;
    }
    register_fd(fd, EPOLLIN);
    return true;
  }
};

template class Dispatcher<DispatcherGATT>;

#pragma once

#include "device_backend_gatt.h"
#include "dispatcher.h"
#include "dispatcher_registry.h"

class DispatcherGATT : public Dispatcher<DispatcherGATT> {
  friend class Singleton<DispatcherGATT>;
  friend class Dispatcher<DispatcherGATT>;

 public:
  const char *type() const override {
    return "gatt";
  }

  void handle_event(EpollPayload *, uint32_t events) override {
    if (events & EPOLLIN) {
      DeviceBackendGATT::instance().pump_messages();
    }
  }

 protected:
  bool on_init() override {
    int fd = DeviceBackendGATT::instance().fd();
    if (fd < 0) {
      return false;
    }
    register_fd(fd, EPOLLIN);
    return true;
  }
};

template class Dispatcher<DispatcherGATT>;

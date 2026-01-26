#pragma once

#include "device_backend_gatt.h"
#include "dispatcher.h"
#include "dispatcher_registry.h"

class DispatcherGATT : public Dispatcher<DispatcherGATT> {
 public:
  const char *type() const override {
    return "gatt";
  }

  void init() override {
    auto &gatt = DeviceBackendGATT::instance();
    gatt.ensure_initialized();
    int fd = DeviceBackendGATT::instance().fd();
    if (fd < 0) {
      std::fprintf(stderr, "DispatcherGATT::init: invalid fd=%d\n", fd);
      return;
    }
    register_fd(fd, EPOLLIN);
  }

  void handle_event(EpollPayload *, uint32_t events) override {
    if (events & EPOLLIN) {
      DeviceBackendGATT::instance().pump_messages();
    }
  }
};

template class Dispatcher<DispatcherGATT>;

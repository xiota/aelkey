#pragma once

#include <libusb-1.0/libusb.h>
#include <poll.h>
#include <sys/epoll.h>

#include "device_backend_libusb.h"
#include "dispatcher.h"

class DispatcherLibUSB : public Dispatcher<DispatcherLibUSB> {
  friend class Singleton<DispatcherLibUSB>;
  friend class Dispatcher<DispatcherLibUSB>;

 protected:
  DispatcherLibUSB() = default;
  ~DispatcherLibUSB() = default;

  bool on_init() override {
    auto &backend = DeviceBackendLibUSB::instance();

    if (!backend.lazy_init()) {
      return false;
    }

    libusb_context *ctx = backend.context();
    if (!ctx) {
      return false;
    }

    libusb_set_pollfd_notifiers(
        ctx,
        [](int fd, short events, void *user_data) {
          static_cast<DispatcherLibUSB *>(user_data)->on_add_fd(fd, events);
        },
        [](int fd, void *user_data) {
          static_cast<DispatcherLibUSB *>(user_data)->on_remove_fd(fd);
        },
        this
    );

    return true;
  }

 public:
  const char *type() const override {
    return "libusb";
  }

  void on_add_fd(int fd, short events) {
    if (get_payload(fd)) {
      return;
    }

    uint32_t evmask = 0;
    if (events & POLLIN) {
      evmask |= EPOLLIN;
    }
    if (events & POLLOUT) {
      evmask |= EPOLLOUT;
    }

    register_fd(fd, evmask);
  }

  void on_remove_fd(int fd) {
    if (get_payload(fd)) {
      unregister_fd(fd);
    }
  }

  void handle_event(EpollPayload *, uint32_t) override {
    auto &backend = DeviceBackendLibUSB::instance();

    timeval tv{ 0, 0 };
    libusb_handle_events_timeout_completed(backend.context(), &tv, nullptr);
  }
};

template class Dispatcher<DispatcherLibUSB>;

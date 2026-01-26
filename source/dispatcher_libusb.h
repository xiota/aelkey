#pragma once

#include <unordered_map>

#include <libusb-1.0/libusb.h>
#include <poll.h>
#include <sys/epoll.h>

#include "dispatcher.h"

class DispatcherLibUSB : public Dispatcher<DispatcherLibUSB> {
  friend class Singleton<DispatcherLibUSB>;

 protected:
  DispatcherLibUSB() = default;
  ~DispatcherLibUSB() {
    if (libusb_) {
      libusb_exit(libusb_);
      libusb_ = nullptr;
    }
  }

 public:
  const char *type() const override {
    return "libusb";
  }

  void ensure_initialized() {
    if (!libusb_) {
      if (libusb_init(&libusb_) != 0) {
        throw std::runtime_error("Failed to init libusb");
      }
    }

    // Install pollfd notifiers
    libusb_set_pollfd_notifiers(
        libusb_,
        [](int fd, short events, void *user_data) {
          static_cast<DispatcherLibUSB *>(user_data)->on_add_fd(fd, events);
        },
        [](int fd, void *user_data) {
          static_cast<DispatcherLibUSB *>(user_data)->on_remove_fd(fd);
        },
        this
    );
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

  libusb_device_handle *open_device(uint16_t vendor, uint16_t product) {
    ensure_initialized();

    libusb_device_handle *handle = libusb_open_device_with_vid_pid(libusb_, vendor, product);

    if (!handle) {
      std::fprintf(stderr, "Failed to open libusb device %04x:%04x\n", vendor, product);
      return nullptr;
    }

    return handle;
  }

  int claim_interface(libusb_device_handle *devh, int iface) {
    if (iface < 0) {
      iface = 0;
    }

    // Detach kernel driver if needed
    if (libusb_kernel_driver_active(devh, iface) == 1) {
      int d = libusb_detach_kernel_driver(devh, iface);
      if (d != 0) {
        std::fprintf(stderr, "Failed to detach kernel driver: %s\n", libusb_error_name(d));
        return d;
      }
    }

    // Claim the interface
    int r = libusb_claim_interface(devh, iface);
    if (r != 0) {
      std::fprintf(stderr, "Failed to claim interface %d: %s\n", iface, libusb_error_name(r));
      return r;
    }

    return 0;
  }

  void handle_event(EpollPayload *, uint32_t) {
    timeval tv{ 0, 0 };
    libusb_handle_events_timeout_completed(libusb_, &tv, nullptr);
  }

 private:
  libusb_context *libusb_ = nullptr;
};

template class Dispatcher<DispatcherLibUSB>;

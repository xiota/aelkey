#include "backend_libusb.h"

#include <cstdio>
#include <cstdlib>
#include <format>
#include <poll.h>

#include "aelkey_state.h"
#include "dispatcher_vulgate.h"
#include "utils/signal.h"

BackendLibUsb::~BackendLibUsb() {
  for (auto *meta : active_transfers_) {
    delete meta;
  }
  active_transfers_.clear();

  for (auto &[id, handle] : devices_) {
    if (handle) {
      libusb_close(handle);
    }
  }
  devices_.clear();

  if (libusb_) {
    libusb_exit(libusb_);
    libusb_ = nullptr;
  }
  AelkeyState::instance().loop_safe_to_stop = true;
}

bool BackendLibUsb::ensure_context() {
  if (libusb_) {
    return true;
  }

  if (libusb_init(&libusb_) == 0) {
    libusb_set_pollfd_notifiers(
        libusb_,
        [](int fd, short events, void *user_data) {
          static_cast<BackendLibUsb *>(user_data)->on_add_pollfd(fd, events);
        },
        [](int fd, void *user_data) {
          static_cast<BackendLibUsb *>(user_data)->on_remove_pollfd(fd);
        },
        this
    );

    const libusb_pollfd **pollfds = libusb_get_pollfds(libusb_);
    if (pollfds) {
      for (const libusb_pollfd **p = pollfds; *p != nullptr; ++p) {
        on_add_pollfd((*p)->fd, (*p)->events);
      }
      libusb_free_pollfds(pollfds);
    }

    return true;
  }

  return false;
}

libusb_device_handle *BackendLibUsb::get_handle(const std::string &id) const {
  auto it = devices_.find(id);
  return (it != devices_.end()) ? it->second : nullptr;
}

bool BackendLibUsb::open_device(
    const std::string &id,
    libusb_device *dev,
    libusb_device_handle *&handle_out
) {
  if (!ensure_context()) {
    return false;
  }

  libusb_device_handle *handle = nullptr;
  if (libusb_open(dev, &handle) == 0 && handle) {
    devices_[id] = handle;
    handle_out = handle;
    return true;
  }
  return false;
}

bool BackendLibUsb::close_device(const std::string &id) {
  auto it = devices_.find(id);
  if (it == devices_.end()) {
    return false;
  }

  libusb_device_handle *handle = it->second;
  if (handle) {
    libusb_config_descriptor *cfg = nullptr;
    libusb_device *dev = libusb_get_device(handle);

    if (libusb_get_active_config_descriptor(dev, &cfg) == 0 && cfg) {
      for (int i = 0; i < cfg->bNumInterfaces; ++i) {
        libusb_release_interface(handle, i);
      }
      libusb_free_config_descriptor(cfg);
    }

    libusb_close(handle);
  }

  devices_.erase(it);
  return true;
}

int BackendLibUsb::claim_interface(libusb_device_handle *devh, int iface) {
  if (iface < 0) {
    iface = 0;
  }

  if (libusb_kernel_driver_active(devh, iface) == 1) {
    int d = libusb_detach_kernel_driver(devh, iface);
    if (d != 0) {
      std::fprintf(
          stderr, "libusb: failed to detach kernel driver: %s\n", libusb_error_name(d)
      );
      return d;
    }
  }

  int r = libusb_claim_interface(devh, iface);
  if (r != 0) {
    std::fprintf(
        stderr, "libusb: failed to claim interface %d: %s\n", iface, libusb_error_name(r)
    );
    return r;
  }

  return 0;
}

bool BackendLibUsb::release_interfaces(libusb_device_handle *devh) {
  if (!devh) {
    return false;
  }
  libusb_config_descriptor *cfg = nullptr;
  libusb_device *dev = libusb_get_device(devh);
  if (libusb_get_active_config_descriptor(dev, &cfg) == 0 && cfg) {
    for (int i = 0; i < cfg->bNumInterfaces; ++i) {
      libusb_release_interface(devh, i);
    }
    libusb_free_config_descriptor(cfg);
    return true;
  }
  return false;
}

void BackendLibUsb::on_add_pollfd(int fd, short events) {
  uint32_t evmask = 0;
  if (events & POLLIN) {
    evmask |= EPOLLIN;
  }
  if (events & POLLOUT) {
    evmask |= EPOLLOUT;
  }

  DispatcherCb cb;
  cb.native = [this]() {
    timeval tv{ 0, 0 };
    libusb_handle_events_timeout_completed(libusb_, &tv, nullptr);
  };

  DispatcherVulgate::instance().register_device_fd(
      fd, evmask | EPOLLHUP | EPOLLERR, std::move(cb), "libusb_poll"
  );
}

void BackendLibUsb::on_remove_pollfd(int fd) {
  DispatcherVulgate::instance().unregister_device_fd(fd);
}

void LIBUSB_CALL BackendLibUsb::on_transfer_complete(libusb_transfer *transfer) {
  if (!transfer || !transfer->user_data) {
    return;
  }

  auto *meta = static_cast<TransferRAII *>(transfer->user_data);
  BackendLibUsb::instance().sig_usb_event_.emit(UsbEvent{ meta->device_id, transfer });
}

UsbSyncResult BackendLibUsb::bulk_transfer(
    const std::string &device,
    int endpoint,
    int size,
    int timeout,
    bool is_in,
    const std::string &out_data
) {
  UsbSyncResult r;
  r.device = device;

  libusb_device_handle *handle = get_handle(device);
  if (!handle) {
    r.status = libusb_error_name(LIBUSB_ERROR_NO_DEVICE);
    r.size = 0;
    return r;
  }

  int status = 0;
  int transferred = 0;

  if (is_in) {
    std::vector<unsigned char> buf(size);
    status = libusb_bulk_transfer(handle, endpoint, buf.data(), size, &transferred, timeout);
    if (status == 0 && transferred > 0) {
      r.data.assign(
          reinterpret_cast<char *>(buf.data()),
          reinterpret_cast<char *>(buf.data()) + transferred
      );
    }

  } else {
    std::string data = out_data;
    if (data.size() > static_cast<std::size_t>(size)) {
      data.resize(static_cast<std::size_t>(size));
    }

    status = libusb_bulk_transfer(
        handle,
        endpoint,
        reinterpret_cast<unsigned char *>(data.data()),
        static_cast<int>(data.size()),
        &transferred,
        timeout
    );

    if (status == 0 && transferred > 0) {
      r.data.assign(data.data(), data.data() + transferred);
    }
  }

  r.size = transferred;
  r.status = (status == 0) ? "ok" : libusb_error_name(status);
  return r;
}

UsbSyncResult BackendLibUsb::control_transfer(
    const std::string &device,
    int request_type,
    int request,
    int value,
    int index,
    int length,
    int timeout,
    bool is_in,
    const std::string &out_data
) {
  UsbSyncResult r;
  r.device = device;

  libusb_device_handle *handle = get_handle(device);
  if (!handle) {
    r.status = libusb_error_name(LIBUSB_ERROR_NO_DEVICE);
    r.size = 0;
    return r;
  }

  int status = 0;
  int transferred = 0;

  if (is_in) {
    std::vector<unsigned char> buf(static_cast<std::size_t>(length));
    status = libusb_control_transfer(
        handle,
        static_cast<uint8_t>(request_type),
        static_cast<uint8_t>(request),
        static_cast<uint16_t>(value),
        static_cast<uint16_t>(index),
        buf.data(),
        static_cast<uint16_t>(length),
        static_cast<unsigned int>(timeout)
    );

    transferred = (status >= 0) ? status : 0;
    if (transferred > 0) {
      r.data.assign(
          reinterpret_cast<char *>(buf.data()),
          reinterpret_cast<char *>(buf.data()) + transferred
      );
    }

  } else {
    std::string data = out_data;
    std::size_t out_len = data.size();
    if (out_len > static_cast<std::size_t>(length)) {
      out_len = static_cast<std::size_t>(length);
    }

    status = libusb_control_transfer(
        handle,
        static_cast<uint8_t>(request_type),
        static_cast<uint8_t>(request),
        static_cast<uint16_t>(value),
        static_cast<uint16_t>(index),
        reinterpret_cast<unsigned char *>(data.data()),
        static_cast<uint16_t>(out_len),
        static_cast<unsigned int>(timeout)
    );

    transferred = (status >= 0) ? status : 0;
  }

  r.size = transferred;
  r.status = (status == 0) ? "ok" : libusb_error_name(status);
  return r;
}

UsbSyncResult BackendLibUsb::interrupt_transfer(
    const std::string &device,
    int endpoint,
    int size,
    int timeout,
    bool is_in,
    const std::string &out_data
) {
  UsbSyncResult r;
  r.device = device;

  libusb_device_handle *handle = get_handle(device);
  if (!handle) {
    r.status = libusb_error_name(LIBUSB_ERROR_NO_DEVICE);
    r.size = 0;
    return r;
  }

  int status = 0;
  int transferred = 0;

  if (is_in) {
    std::vector<unsigned char> buf(size);
    status =
        libusb_interrupt_transfer(handle, endpoint, buf.data(), size, &transferred, timeout);
    if (status == 0 && transferred > 0) {
      r.data.assign(
          reinterpret_cast<char *>(buf.data()),
          reinterpret_cast<char *>(buf.data()) + transferred
      );
    }

  } else {
    std::string data = out_data;
    if (data.size() > static_cast<std::size_t>(size)) {
      data.resize(static_cast<std::size_t>(size));
    }

    status = libusb_interrupt_transfer(
        handle,
        endpoint,
        reinterpret_cast<unsigned char *>(data.data()),
        static_cast<int>(data.size()),
        &transferred,
        timeout
    );

    if (status == 0 && transferred > 0) {
      r.data.assign(data.data(), data.data() + transferred);
    }
  }

  r.size = transferred;
  r.status = (status == 0) ? "ok" : libusb_error_name(status);
  return r;
}

UsbSubmitResult BackendLibUsb::submit_transfer(
    const std::string &device,
    int endpoint,
    const std::string &type_str,
    int size,
    int timeout
) {
  UsbSubmitResult sr;
  sr.status = "ok";

  libusb_device_handle *handle = get_handle(device);
  if (!handle) {
    sr.status = libusb_error_name(LIBUSB_ERROR_NO_DEVICE);
    return sr;
  }

  int type = LIBUSB_TRANSFER_TYPE_INTERRUPT;
  if (type_str == "bulk") {
    type = LIBUSB_TRANSFER_TYPE_BULK;
  } else if (type_str == "control") {
    type = LIBUSB_TRANSFER_TYPE_CONTROL;
  } else if (type_str == "iso") {
    type = LIBUSB_TRANSFER_TYPE_ISOCHRONOUS;
  }

  auto *meta = new TransferRAII;
  meta->device_id = device;

  meta->xfer = libusb_alloc_transfer(0);
  if (!meta->xfer) {
    delete meta;
    sr.status = libusb_error_name(LIBUSB_ERROR_NO_MEM);
    return sr;
  }

  meta->buffer = static_cast<unsigned char *>(std::malloc(size));
  if (!meta->buffer) {
    delete meta;
    sr.status = libusb_error_name(LIBUSB_ERROR_NO_MEM);
    return sr;
  }

  libusb_transfer *xfer = meta->xfer;

  xfer->dev_handle = handle;
  xfer->endpoint = static_cast<uint8_t>(endpoint);
  xfer->type = static_cast<uint8_t>(type);
  xfer->timeout = static_cast<unsigned int>(timeout);
  xfer->buffer = meta->buffer;
  xfer->length = size;

  xfer->user_data = meta;
  xfer->callback = on_transfer_complete;

  int status = libusb_submit_transfer(xfer);
  if (status != 0) {
    delete meta;
    sr.status = libusb_error_name(status);
    sr.handle = nullptr;
    return sr;
  }

  AelkeyState::instance().loop_safe_to_stop = false;
  active_transfers_.insert(meta);

  sr.handle = xfer;
  sr.status = "ok";
  return sr;
}

bool BackendLibUsb::cancel_transfer(libusb_transfer *handle) {
  auto *xfer = static_cast<libusb_transfer *>(handle);
  if (!xfer) {
    return false;
  }
  int rc = libusb_cancel_transfer(xfer);
  return rc == 0;
}

bool BackendLibUsb::resubmit_transfer(libusb_transfer *handle) {
  auto *xfer = static_cast<libusb_transfer *>(handle);
  if (!xfer) {
    return false;
  }

  auto &state = AelkeyState::instance();
  if (!state.loop_running || state.loop_should_stop) {
    remove_raii(xfer);
    return false;
  }

  int rc = libusb_submit_transfer(xfer);
  if (rc != 0) {
    remove_raii(xfer);
  } else {
    auto *meta = static_cast<TransferRAII *>(xfer->user_data);
    active_transfers_.insert(meta);
    AelkeyState::instance().loop_safe_to_stop = false;
  }
  return rc == 0;
}

void BackendLibUsb::remove_raii(libusb_transfer *xfer) {
  if (!xfer || !xfer->user_data) {
    return;
  }
  auto *meta = static_cast<TransferRAII *>(xfer->user_data);
  active_transfers_.erase(meta);
  delete meta;

  if (active_transfers_.size() == 0) {
    AelkeyState::instance().loop_safe_to_stop = true;
  }
}

std::string BackendLibUsb::clear_halt(const std::string &device, int endpoint) {
  libusb_device_handle *handle = get_handle(device);
  if (!handle) {
    return libusb_error_name(LIBUSB_ERROR_NO_DEVICE);
  }

  int status = libusb_clear_halt(handle, static_cast<uint8_t>(endpoint));
  return (status == 0) ? "ok" : libusb_error_name(status);
}

std::string BackendLibUsb::reset_device(const std::string &device) {
  libusb_device_handle *handle = get_handle(device);
  if (!handle) {
    return libusb_error_name(LIBUSB_ERROR_NO_DEVICE);
  }

  int status = libusb_reset_device(handle);
  return (status == 0) ? "ok" : libusb_error_name(status);
}

std::string BackendLibUsb::set_configuration(const std::string &device, int config) {
  libusb_device_handle *handle = get_handle(device);
  if (!handle) {
    return libusb_error_name(LIBUSB_ERROR_NO_DEVICE);
  }

  int status = libusb_set_configuration(handle, config);
  return (status == 0) ? "ok" : libusb_error_name(status);
}

std::string BackendLibUsb::set_interface_alt_setting(
    const std::string &device,
    int interface_number,
    int alt_setting
) {
  libusb_device_handle *handle = get_handle(device);
  if (!handle) {
    return libusb_error_name(LIBUSB_ERROR_NO_DEVICE);
  }

  int status = libusb_set_interface_alt_setting(
      handle, static_cast<int>(interface_number), static_cast<int>(alt_setting)
  );
  return (status == 0) ? "ok" : libusb_error_name(status);
}

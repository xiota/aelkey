#include "device_in_libusb.h"

#include <cstdio>
#include <format>
#include <map>
#include <string>

#include <libusb-1.0/libusb.h>
#include <poll.h>
#include <readerwriterqueue.h>

#include <sol/sol.hpp>

#include "aelkey_state.h"
#include "backend_udev.h"
#include "device_declarations.h"
#include "dispatcher_event.h"
#include "dispatcher_vulgate.h"
#include "manager_device_in.h"
#include "utils/signal.h"

// Map libusb_transfer_type enum → string
static const char *transfer_type_to_string(uint8_t type) {
  switch (type) {
    case LIBUSB_TRANSFER_TYPE_CONTROL:
      return "control";
    case LIBUSB_TRANSFER_TYPE_ISOCHRONOUS:
      return "iso";
    case LIBUSB_TRANSFER_TYPE_BULK:
      return "bulk";
    case LIBUSB_TRANSFER_TYPE_INTERRUPT:
      return "interrupt";
    default:
      return "unknown";
  }
}

// Map libusb_transfer_status enum → string
static const char *transfer_status_to_string(libusb_transfer_status status) {
  switch (status) {
    case LIBUSB_TRANSFER_COMPLETED:
      return "ok";
    case LIBUSB_TRANSFER_ERROR:
      return "error";
    case LIBUSB_TRANSFER_TIMED_OUT:
      return "timeout";
    case LIBUSB_TRANSFER_CANCELLED:
      return "cancelled";
    case LIBUSB_TRANSFER_STALL:
      return "stall";
    case LIBUSB_TRANSFER_NO_DEVICE:
      return "no_device";
    case LIBUSB_TRANSFER_OVERFLOW:
      return "overflow";
    default:
      return "unknown";
  }
}

static bool matches_decl(const InputDecl &decl, const libusb_device_descriptor &desc) {
  bool vidpid_ok = decl.vid_pid.empty();
  for (auto &[v, p] : decl.vid_pid) {
    bool vendor_ok = (v == 0 || v == desc.idVendor);
    bool product_ok = (p == 0 || p == desc.idProduct);
    if (vendor_ok && product_ok) {
      vidpid_ok = true;
      break;
    }
  }
  if (!vidpid_ok) {
    return false;
  }

  if (decl.version != 0 && decl.version != desc.bcdDevice) {
    return false;
  }

  return true;
}

// libusb async callback → enqueue into DeviceInLibUsb queue
static void LIBUSB_CALL on_transfer_complete(libusb_transfer *transfer) {
  if (!transfer || !transfer->user_data) {
    return;
  }

  auto *meta = static_cast<TransferRAII *>(transfer->user_data);
  DeviceInLibUsb::instance().enqueue_event(meta->device_id, transfer);
}

DeviceInLibUsb::DeviceInLibUsb() {
  tok_udev_event_ =
      BackendUdev::instance().sig_udev_event_.subscribe([this](const UdevEvent &ev) {
        if (ev.subsystem != "usb") {
          return;
        }

        if (ev.devtype != "usb_device") {
          return;
        }

        if (ev.busnum.empty() || ev.devnum.empty()) {
          return;
        }

        std::string inst_node =
            std::format("usb:{:03}-{:03}", std::stoi(ev.busnum), std::stoi(ev.devnum));

        auto &state = AelkeyState::instance();

        if (ev.action == "add") {
          libusb_device_descriptor desc{};
          desc.idVendor = static_cast<uint16_t>(strtol(ev.vid.c_str(), nullptr, 16));
          desc.idProduct = static_cast<uint16_t>(strtol(ev.pid.c_str(), nullptr, 16));

          for (auto &decl : state.input_decls) {
            if (decl.type != "libusb") {
              continue;
            }

            if (!matches_decl(decl, desc)) {
              continue;
            }

            if (ManagerDeviceIn::instance().attach(inst_node, decl)) {
              break;
            }
          }

        } else if (ev.action == "remove") {
          for (auto &decl : state.input_decls) {
            if (decl.type != "libusb") {
              continue;
            }

            if (decl.devnode != inst_node) {
              continue;
            }
            if (ManagerDeviceIn::instance().detach(decl.id)) {
              break;
            }
          }
        }
      });
}

DeviceInLibUsb::~DeviceInLibUsb() {
  for (auto *meta : active_transfers_) {
    delete meta;
  }
  active_transfers_.clear();

  if (libusb_) {
    libusb_exit(libusb_);
    libusb_ = nullptr;
  }

  AelkeyState::instance().loop_safe_to_stop = true;
}

bool DeviceInLibUsb::match(InputDecl &decl, std::string &devnode_out) {
  if (decl.type != "libusb") {
    return false;
  }

  if (decl.vid_pid.empty()) {
    return false;
  }

  devnode_out = decl.id;
  return true;
}

bool DeviceInLibUsb::attach(const std::string &devnode, InputDecl &decl) {
  if (!on_init()) {
    return false;
  }

  libusb_device **list = nullptr;
  ssize_t count = libusb_get_device_list(libusb_, &list);
  if (count < 0) {
    return false;
  }

  uint16_t vendor = 0;
  uint16_t product = 0;

  libusb_device_handle *handle = nullptr;

  for (ssize_t i = 0; i < count; ++i) {
    libusb_device *dev = list[i];
    libusb_device_descriptor desc;

    if (libusb_get_device_descriptor(dev, &desc) != 0) {
      continue;
    }

    if (!matches_decl(decl, desc)) {
      continue;
    }

    if (libusb_open(dev, &handle) == 0 && handle) {
      vendor = desc.idVendor;
      product = desc.idProduct;

      uint8_t bus = libusb_get_bus_number(dev);
      uint8_t addr = libusb_get_device_address(dev);
      decl.devnode = std::format("usb:{:03}-{:03}", bus, addr);

      break;
    }
  }

  libusb_free_device_list(list, 1);
  if (!handle) {
    return false;
  }

  if (decl.interfaces.empty()) {
    libusb_config_descriptor *cfg = nullptr;
    if (libusb_get_active_config_descriptor(libusb_get_device(handle), &cfg) == 0 && cfg) {
      for (int i = 0; i < cfg->bNumInterfaces; ++i) {
        claim_interface(handle, i);
      }
      libusb_free_config_descriptor(cfg);
    }

  } else {
    for (int iface : decl.interfaces) {
      claim_interface(handle, iface);
    }
  }

  decl.vendor = vendor;
  decl.product = product;

  devices_[decl.id] = handle;
  input_decls_[decl.id] = decl;

  return true;
}

bool DeviceInLibUsb::detach(const std::string &id) {
  auto it = devices_.find(id);
  if (it == devices_.end()) {
    return false;
  }

  libusb_device_handle *handle = it->second;
  if (!handle) {
    devices_.erase(it);
    input_decls_.erase(id);
    return false;
  }

  libusb_config_descriptor *cfg = nullptr;
  libusb_device *dev = libusb_get_device(handle);

  if (libusb_get_active_config_descriptor(dev, &cfg) == 0 && cfg) {
    for (int i = 0; i < cfg->bNumInterfaces; ++i) {
      libusb_release_interface(handle, i);
    }
    libusb_free_config_descriptor(cfg);
  }

  libusb_close(handle);
  devices_.erase(it);
  input_decls_.erase(id);

  return true;
}

bool DeviceInLibUsb::on_init() {
  if (libusb_) {
    return true;
  }

  if (libusb_init(&libusb_) == 0) {
    libusb_set_pollfd_notifiers(
        libusb_,
        [](int fd, short events, void *user_data) {
          static_cast<DeviceInLibUsb *>(user_data)->on_add_pollfd(fd, events);
        },
        [](int fd, void *user_data) {
          static_cast<DeviceInLibUsb *>(user_data)->on_remove_pollfd(fd);
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

    if (dispatch_fd_ < 0) {
      DispatcherCb cb;
      cb.native = [this]() { this->pump_messages(); };
      cb.oneshot = false;

      dispatch_fd_ = DispatcherEvent::instance().create(cb);
      if (dispatch_fd_ < 0) {
        std::fprintf(stderr, "libusb: failed to create event dispatcher\n");
      }
    }

    return true;
  }

  return false;
}

void DeviceInLibUsb::on_add_pollfd(int fd, short events) {
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

void DeviceInLibUsb::on_remove_pollfd(int fd) {
  DispatcherVulgate::instance().unregister_device_fd(fd);
}

int DeviceInLibUsb::claim_interface(libusb_device_handle *devh, int iface) {
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

libusb_context *DeviceInLibUsb::context() const {
  return libusb_;
}

libusb_device_handle *DeviceInLibUsb::get_handle(const std::string &id) const {
  auto it = devices_.find(id);
  return (it != devices_.end()) ? it->second : nullptr;
}

void DeviceInLibUsb::enqueue_event(const std::string &id, libusb_transfer *transfer) {
  UsbEvent ev{ id, transfer };
  queue_.enqueue(ev);

  if (dispatch_fd_ >= 0) {
    DispatcherEvent::instance().trigger(dispatch_fd_);
  }
}

UsbSyncResult DeviceInLibUsb::bulk_transfer(
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

UsbSyncResult DeviceInLibUsb::control_transfer(
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
    // OUT control: original behavior is empty data
  }

  r.size = transferred;
  r.status = (status == 0) ? "ok" : libusb_error_name(status);
  return r;
}

UsbSyncResult DeviceInLibUsb::interrupt_transfer(
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

UsbSubmitResult DeviceInLibUsb::submit_transfer(
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

  auto it = input_decls_.find(device);
  if (it == input_decls_.end()) {
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

  // RAII object
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

bool DeviceInLibUsb::cancel_transfer(libusb_transfer *handle) {
  auto *xfer = static_cast<libusb_transfer *>(handle);
  if (!xfer) {
    return false;
  }
  int rc = libusb_cancel_transfer(xfer);
  return rc == 0;
}

bool DeviceInLibUsb::resubmit_transfer(libusb_transfer *handle) {
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

void DeviceInLibUsb::remove_raii(libusb_transfer *xfer) {
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

std::string DeviceInLibUsb::clear_halt(const std::string &device, int endpoint) {
  libusb_device_handle *handle = get_handle(device);
  if (!handle) {
    return libusb_error_name(LIBUSB_ERROR_NO_DEVICE);
  }

  int status = libusb_clear_halt(handle, static_cast<uint8_t>(endpoint));
  return (status == 0) ? "ok" : libusb_error_name(status);
}

std::string DeviceInLibUsb::reset_device(const std::string &device) {
  libusb_device_handle *handle = get_handle(device);
  if (!handle) {
    return libusb_error_name(LIBUSB_ERROR_NO_DEVICE);
  }

  int status = libusb_reset_device(handle);
  return (status == 0) ? "ok" : libusb_error_name(status);
}

std::string DeviceInLibUsb::set_configuration(const std::string &device, int config) {
  libusb_device_handle *handle = get_handle(device);
  if (!handle) {
    return libusb_error_name(LIBUSB_ERROR_NO_DEVICE);
  }

  int status = libusb_set_configuration(handle, config);
  return (status == 0) ? "ok" : libusb_error_name(status);
}

std::string DeviceInLibUsb::set_interface_alt_setting(
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

void DeviceInLibUsb::pump_messages() {
  auto &state = AelkeyState::instance();
  sol::state_view lua(state.lua_vm);

  UsbEvent ev;
  while (queue_.try_dequeue(ev)) {
    auto it_decl = input_decls_.find(ev.id);
    if (it_decl == input_decls_.end()) {
      if (ev.transfer) {
        remove_raii(ev.transfer);
      }
      continue;
    }

    InputDecl &decl = it_decl->second;
    if (decl.on_event.empty()) {
      if (ev.transfer) {
        remove_raii(ev.transfer);
      }
      continue;
    }

    sol::object cb_obj = lua[decl.on_event];
    if (!cb_obj.is<sol::function>()) {
      if (ev.transfer) {
        remove_raii(ev.transfer);
      }
      continue;
    }

    sol::function cb = cb_obj.as<sol::function>();

    sol::table e = lua.create_table();
    e["device"] = decl.id;
    e["data"] = std::string_view(
        reinterpret_cast<const char *>(ev.transfer->buffer), ev.transfer->actual_length
    );
    e["size"] = static_cast<int>(ev.transfer->actual_length);
    e["endpoint"] = static_cast<int>(ev.transfer->endpoint);
    e["transfer"] = transfer_type_to_string(ev.transfer->type);
    e["status"] = transfer_status_to_string(ev.transfer->status);

    sol::protected_function pf = cb;
    sol::protected_function_result res = pf(e);

    bool lua_ok = res.valid();

    switch (ev.transfer->status) {
      case LIBUSB_TRANSFER_COMPLETED:
      case LIBUSB_TRANSFER_OVERFLOW:
      case LIBUSB_TRANSFER_TIMED_OUT: {
        if (lua_ok) {
          resubmit_transfer(ev.transfer);

        } else {
          remove_raii(ev.transfer);
        }
        break;
      }

      case LIBUSB_TRANSFER_NO_DEVICE: {
        ManagerDeviceIn::instance().detach(decl.id);
        remove_raii(ev.transfer);
        break;
      }

      case LIBUSB_TRANSFER_CANCELLED:
      case LIBUSB_TRANSFER_ERROR:
      default: {
        libusb_device_descriptor desc{};
        libusb_device_handle *handle = get_handle(decl.id);

        int rc = -1;
        if (handle) {
          rc = libusb_get_device_descriptor(libusb_get_device(handle), &desc);
        }
        if (rc != 0) {
          ManagerDeviceIn::instance().detach(decl.id);
        }

        remove_raii(ev.transfer);
        break;
      }
    }

    if (!lua_ok) {
      sol::error err = res;
      std::fprintf(stderr, "Lua libusb callback error: %s\n", err.what());
    }
  }
}

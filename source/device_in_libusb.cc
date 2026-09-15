#include "device_in_libusb.h"

#include <cstdio>
#include <format>
#include <map>
#include <string>

#include <libusb-1.0/libusb.h>
#include <readerwriterqueue.h>

#include <sol/sol.hpp>

#include "aelkey_state.h"
#include "backend_libusb.h"
#include "backend_udev.h"
#include "device_declarations.h"
#include "dispatcher_event.h"
#include "manager_device.h"
#include "utils/lua_helpers.h"
#include "utils/signal.h"
#include "utils/time.h"

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

            if (ManagerDevice::instance().attach_input(inst_node, decl)) {
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
            if (ManagerDevice::instance().detach_output(decl.id)) {
              break;
            }
          }
        }
      });
}

bool DeviceInLibUsb::on_init() {
  auto &backend = BackendLibUsb::instance();
  if (!backend.ensure_context()) {
    return false;
  }

  tok_usb_event_ = backend.sig_usb_event_.subscribe([this](const UsbEvent &ev) {
    this->enqueue_event(ev.id, ev.transfer);
  });

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

  auto &backend = BackendLibUsb::instance();
  libusb_context *ctx = backend.context();

  libusb_device **list = nullptr;
  ssize_t count = libusb_get_device_list(ctx, &list);
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

    if (backend.open_device(decl.id, dev, handle)) {
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
        backend.claim_interface(handle, i);
      }
      libusb_free_config_descriptor(cfg);
    }

  } else {
    for (int iface : decl.interfaces) {
      backend.claim_interface(handle, iface);
    }
  }

  decl.vendor = vendor;
  decl.product = product;

  input_decls_[decl.id] = decl;

  return true;
}

bool DeviceInLibUsb::detach(const std::string &id) {
  auto it = input_decls_.find(id);
  if (it == input_decls_.end()) {
    return false;
  }

  input_decls_.erase(it);
  BackendLibUsb::instance().close_device(id);

  return true;
}

void DeviceInLibUsb::enqueue_event(const std::string &id, libusb_transfer *transfer) {
  UsbEvent ev{ id, transfer };
  queue_.enqueue(ev);

  if (dispatch_fd_ >= 0) {
    DispatcherEvent::instance().trigger(dispatch_fd_);
  }
}

void DeviceInLibUsb::pump_messages() {
  auto &state = AelkeyState::instance();
  sol::state_view lua(state.lua_vm);
  auto &backend = BackendLibUsb::instance();

  UsbEvent ev;
  while (queue_.try_dequeue(ev)) {
    auto it_decl = input_decls_.find(ev.id);
    if (it_decl == input_decls_.end()) {
      if (ev.transfer) {
        backend.remove_raii(ev.transfer);
      }
      continue;
    }

    InputDecl &decl = it_decl->second;
    if (decl.on_event.empty()) {
      if (ev.transfer) {
        backend.remove_raii(ev.transfer);
      }
      continue;
    }

    sol::object cb_obj = lua[decl.on_event];
    if (!cb_obj.is<sol::function>()) {
      if (ev.transfer) {
        backend.remove_raii(ev.transfer);
      }
      continue;
    }

    sol::function cb = cb_obj.as<sol::function>();

    UsbEventPayload payload{
      .device = decl.id,
      .data = std::string_view(
          reinterpret_cast<const char *>(ev.transfer->buffer), ev.transfer->actual_length
      ),
      .size = static_cast<int>(ev.transfer->actual_length),
      .endpoint = static_cast<int>(ev.transfer->endpoint),
      .transfer = transfer_type_to_string(ev.transfer->type),
      .status = transfer_status_to_string(ev.transfer->status),
      .timestamp = AelkeyUtil::now("us"),
    };

    sol::table e = payload.to_lua(lua);

    sol::protected_function pf = cb;
    sol::protected_function_result res = pf(e);

    bool lua_ok = res.valid();

    switch (ev.transfer->status) {
      case LIBUSB_TRANSFER_COMPLETED:
      case LIBUSB_TRANSFER_OVERFLOW:
      case LIBUSB_TRANSFER_TIMED_OUT: {
        if (lua_ok) {
          backend.resubmit_transfer(ev.transfer);
        } else {
          backend.remove_raii(ev.transfer);
        }
        break;
      }

      case LIBUSB_TRANSFER_NO_DEVICE: {
        ManagerDevice::instance().detach_output(decl.id);
        backend.remove_raii(ev.transfer);
        break;
      }

      case LIBUSB_TRANSFER_CANCELLED:
      case LIBUSB_TRANSFER_ERROR:
      default: {
        libusb_device_descriptor desc{};
        libusb_device_handle *handle = backend.get_handle(decl.id);

        int rc = -1;
        if (handle) {
          rc = libusb_get_device_descriptor(libusb_get_device(handle), &desc);
        }
        if (rc != 0) {
          ManagerDevice::instance().detach_output(decl.id);
        }

        backend.remove_raii(ev.transfer);
        break;
      }
    }

    if (!lua_ok) {
      sol::error err = res;
      std::fprintf(stderr, "Lua libusb callback error: %s\n", err.what());
    }
  }
}

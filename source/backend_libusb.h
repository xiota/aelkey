#pragma once

#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include <libusb-1.0/libusb.h>

#include "aelkey_state.h"
#include "singleton.h"
#include "utils/signal.h"

struct TransferRAII {
  libusb_transfer *xfer = nullptr;
  unsigned char *buffer = nullptr;
  std::string device_id;

  TransferRAII() {
    AelkeyState::instance().increment_active_tasks();
  }

  ~TransferRAII() {
    if (buffer) {
      std::free(buffer);
      buffer = nullptr;
    }
    if (xfer) {
      libusb_free_transfer(xfer);
      xfer = nullptr;
    }
    AelkeyState::instance().decrement_active_tasks();
  }

  TransferRAII(const TransferRAII &) = delete;
  TransferRAII &operator=(const TransferRAII &) = delete;
  TransferRAII(TransferRAII &&) noexcept = default;
  TransferRAII &operator=(TransferRAII &&) noexcept = default;
};

struct UsbEvent {
  std::string id;
  libusb_transfer *transfer;
};

struct UsbSyncResult {
  std::string device;
  std::string status;
  std::vector<char> data;
  int size = 0;
};

struct UsbSubmitResult {
  libusb_transfer *handle = nullptr;
  std::string status;
};

class BackendLibUsb : public Singleton<BackendLibUsb> {
  friend class Singleton<BackendLibUsb>;

 private:
  BackendLibUsb() = default;
  ~BackendLibUsb();

 public:
  bool ensure_context();

  libusb_context *context() const {
    return libusb_;
  }

  libusb_device_handle *get_handle(const std::string &id) const;

  bool
  open_device(const std::string &id, libusb_device *dev, libusb_device_handle *&handle_out);
  bool close_device(const std::string &id);

  int claim_interface(libusb_device_handle *devh, int iface);
  bool release_interfaces(libusb_device_handle *devh);

  void on_add_pollfd(int fd, short events);
  void on_remove_pollfd(int fd);

  // Synchronous transfers
  UsbSyncResult bulk_transfer(
      const std::string &device,
      int endpoint,
      int size,
      int timeout,
      bool is_in,
      const std::string &out_data
  );

  UsbSyncResult control_transfer(
      const std::string &device,
      int request_type,
      int request,
      int value,
      int index,
      int length,
      int timeout,
      bool is_in,
      const std::string &out_data
  );

  UsbSyncResult interrupt_transfer(
      const std::string &device,
      int endpoint,
      int size,
      int timeout,
      bool is_in,
      const std::string &out_data
  );

  // Asynchronous submit + control
  UsbSubmitResult submit_transfer(
      const std::string &device,
      int endpoint,
      const std::string &type_str,
      int size,
      int timeout
  );

  bool cancel_transfer(libusb_transfer *handle);
  bool resubmit_transfer(libusb_transfer *handle);
  void remove_raii(libusb_transfer *xfer);

  // Device-level operations
  std::string clear_halt(const std::string &device, int endpoint);
  std::string reset_device(const std::string &device);
  std::string set_configuration(const std::string &device, int config);
  std::string
  set_interface_alt_setting(const std::string &device, int interface_number, int alt_setting);

 public:
  AelkeyUtil::Signal<void(const UsbEvent &)> sig_usb_event_;

 private:
  static void LIBUSB_CALL on_transfer_complete(libusb_transfer *transfer);

 private:
  libusb_context *libusb_ = nullptr;
  std::map<std::string, libusb_device_handle *> devices_;
  std::unordered_map<libusb_transfer *, std::unique_ptr<TransferRAII>> active_transfers_;
};

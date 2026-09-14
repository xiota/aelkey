#pragma once

#include <map>
#include <string>
#include <unordered_set>
#include <vector>

#include <libudev.h>
#include <libusb-1.0/libusb.h>
#include <readerwriterqueue.h>

#include "backend_udev.h"
#include "device_declarations.h"
#include "device_in.h"
#include "singleton.h"
#include "utils/signal.h"

struct TransferRAII {
  libusb_transfer *xfer = nullptr;
  unsigned char *buffer = nullptr;
  std::string device_id;

  ~TransferRAII() {
    if (buffer) {
      std::free(buffer);
      buffer = nullptr;
    }
    if (xfer) {
      libusb_free_transfer(xfer);
      xfer = nullptr;
    }
  }
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

class DeviceInLibUsb : public DeviceIn, public Singleton<DeviceInLibUsb> {
  friend class Singleton<DeviceInLibUsb>;

 protected:
  DeviceInLibUsb();
  ~DeviceInLibUsb() override;

 public:
  bool match(InputDecl &decl, std::string &devnode_out) override;
  bool attach(const std::string &devnode, InputDecl &decl) override;
  bool detach(const std::string &id) override;

  bool on_init() override;

  int claim_interface(libusb_device_handle *devh, int iface);

  libusb_context *context() const;
  libusb_device_handle *get_handle(const std::string &id) const;

  void on_add_pollfd(int fd, short events);
  void on_remove_pollfd(int fd);

  void enqueue_event(const std::string &id, libusb_transfer *transfer);
  void pump_messages();

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

  // Device-level operations
  std::string clear_halt(const std::string &device, int endpoint);
  std::string reset_device(const std::string &device);
  std::string set_configuration(const std::string &device, int config);
  std::string
  set_interface_alt_setting(const std::string &device, int interface_number, int alt_setting);

 private:
  void remove_raii(libusb_transfer *xfer);

 private:
  libusb_context *libusb_ = nullptr;
  std::map<std::string, libusb_device_handle *> devices_;

  // key = id
  std::map<std::string, InputDecl> input_decls_;

  moodycamel::ReaderWriterQueue<UsbEvent> queue_;
  int dispatch_fd_ = -1;

  AelkeyUtil::Signal<void(const UdevEvent &)>::Connection tok_udev_event_;

  std::unordered_set<TransferRAII *> active_transfers_;
};

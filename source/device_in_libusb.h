#pragma once

#include <map>
#include <string>
#include <vector>

#include <readerwriterqueue.h>

#include "backend_libusb.h"
#include "backend_udev.h"
#include "device_declarations.h"
#include "device_in.h"
#include "singleton.h"
#include "utils/signal.h"

class DeviceInLibUsb : public DeviceIn, public Singleton<DeviceInLibUsb> {
  friend class Singleton<DeviceInLibUsb>;

 protected:
  DeviceInLibUsb();
  ~DeviceInLibUsb() = default;

 public:
  bool match(InputDecl &decl, std::string &devnode_out) override;
  bool attach(const std::string &devnode, InputDecl &decl) override;
  bool detach(const std::string &id) override;

  bool on_init() override;

  void enqueue_event(const std::string &id, libusb_transfer *transfer);
  void pump_messages();

 private:
  moodycamel::ReaderWriterQueue<UsbEvent> queue_;
  int dispatch_fd_ = -1;

  // key = id
  std::map<std::string, InputDecl> input_decls_;

  AelkeyUtil::Signal<void(const UdevEvent &)>::Connection tok_udev_event_;
  AelkeyUtil::Signal<void(const UsbEvent &)>::Connection tok_usb_event_;
};

#include "backend_udev.h"

#include <cstring>
#include <iostream>

#include <unistd.h>

#include "dispatcher_vulgate.h"

BackendUdev::~BackendUdev() {
  if (mon_) {
    udev_monitor_unref(mon_);
    mon_ = nullptr;
  }
  if (udev_ctx_) {
    udev_unref(udev_ctx_);
    udev_ctx_ = nullptr;
  }
  mon_fd_ = -1;
}

bool BackendUdev::on_init() {
  if (udev_ctx_) {
    return true;
  }

  udev_ctx_ = udev_new();
  if (!udev_ctx_) {
    return false;
  }

  mon_ = udev_monitor_new_from_netlink(udev_ctx_, "udev");
  if (!mon_) {
    udev_unref(udev_ctx_);
    udev_ctx_ = nullptr;
    return false;
  }

  udev_monitor_filter_add_match_subsystem_devtype(mon_, "input", nullptr);
  udev_monitor_filter_add_match_subsystem_devtype(mon_, "hidraw", nullptr);
  udev_monitor_filter_add_match_subsystem_devtype(mon_, "usb", nullptr);
  udev_monitor_enable_receiving(mon_);

  mon_fd_ = udev_monitor_get_fd(mon_);
  if (mon_fd_ < 0) {
    udev_monitor_unref(mon_);
    udev_ctx_ = nullptr;
    return false;
  }

  DispatcherCb cb;
  cb.native = [this]() { handle_udev_event(mon_fd_); };
  cb.cleanup = [](int fd_to_clean) {
    // Mon fd cleanup is handled in destructor via unregister_device_fd
  };

  DispatcherVulgate::instance().register_device_fd(
      mon_fd_, EPOLLIN | EPOLLHUP | EPOLLERR, std::move(cb), "udev_monitor"
  );

  return true;
}

void BackendUdev::handle_udev_event(int fd) {
  if (!mon_) {
    return;
  }

  struct udev_device *dev = udev_monitor_receive_device(mon_);
  if (!dev) {
    return;
  }

  const char *action = udev_device_get_action(dev);
  if (action) {
    if (std::strcmp(action, "add") == 0) {
      handle_udev_add(dev);
    } else if (std::strcmp(action, "remove") == 0) {
      handle_udev_remove(dev);
    }
  }

  udev_device_unref(dev);
}

std::string BackendUdev::enumerate_and_match(
    const char *subsystem,
    const std::function<std::string(struct udev_device *)> &matcher
) {
  if (!lazy_init() || !udev_ctx_) {
    return {};
  }

  struct udev_enumerate *enumerate = udev_enumerate_new(udev_ctx_);
  if (!enumerate) {
    return {};
  }

  udev_enumerate_add_match_subsystem(enumerate, subsystem);
  udev_enumerate_scan_devices(enumerate);

  struct udev_list_entry *devices = udev_enumerate_get_list_entry(enumerate);
  struct udev_list_entry *entry;

  udev_list_entry_foreach(entry, devices) {
    const char *path = udev_list_entry_get_name(entry);
    struct udev_device *dev = udev_device_new_from_syspath(udev_ctx_, path);
    if (!dev) {
      continue;
    }

    std::string result = matcher(dev);
    udev_device_unref(dev);

    if (!result.empty()) {
      udev_enumerate_unref(enumerate);
      return result;
    }
  }

  udev_enumerate_unref(enumerate);
  return {};
}

struct udev *BackendUdev::get_udev() const {
  return udev_ctx_;
}

void BackendUdev::handle_udev_add(struct udev_device *dev) {
  const char *subsystem = udev_device_get_subsystem(dev);
  const char *node = udev_device_get_devnode(dev);
  const char *syspath = udev_device_get_syspath(dev);

  if (!subsystem || !syspath) {
    return;
  }

  UdevEvent ev;
  ev.action = "add";
  ev.subsystem = subsystem;
  ev.devnode = node ? node : "";
  ev.syspath = syspath;

  if (std::strcmp(subsystem, "usb") == 0) {
    const char *devtype = udev_device_get_devtype(dev);
    if (devtype) {
      ev.devtype = devtype;
    }

    const char *vid = udev_device_get_property_value(dev, "ID_VENDOR_ID");
    const char *pid = udev_device_get_property_value(dev, "ID_MODEL_ID");
    const char *bus = udev_device_get_property_value(dev, "BUSNUM");
    const char *devnum = udev_device_get_property_value(dev, "DEVNUM");

    if (vid) {
      ev.vid = vid;
    }
    if (pid) {
      ev.pid = pid;
    }
    if (bus) {
      ev.busnum = bus;
    }
    if (devnum) {
      ev.devnum = devnum;
    }
  }

  sig_udev_event_.emit(ev);
}

void BackendUdev::handle_udev_remove(struct udev_device *dev) {
  const char *subsystem = udev_device_get_subsystem(dev);
  const char *node = udev_device_get_devnode(dev);
  const char *syspath = udev_device_get_syspath(dev);

  if (!subsystem || !syspath) {
    return;
  }

  UdevEvent ev;
  ev.action = "remove";
  ev.subsystem = subsystem;
  ev.devnode = node ? node : "";
  ev.syspath = syspath;

  if (std::strcmp(subsystem, "usb") == 0) {
    const char *devtype = udev_device_get_devtype(dev);
    if (devtype) {
      ev.devtype = devtype;
    }

    const char *vid = udev_device_get_property_value(dev, "ID_VENDOR_ID");
    const char *pid = udev_device_get_property_value(dev, "ID_MODEL_ID");
    const char *bus = udev_device_get_property_value(dev, "BUSNUM");
    const char *devnum = udev_device_get_property_value(dev, "DEVNUM");

    if (vid) {
      ev.vid = vid;
    }
    if (pid) {
      ev.pid = pid;
    }
    if (bus) {
      ev.busnum = bus;
    }
    if (devnum) {
      ev.devnum = devnum;
    }
  }

  sig_udev_event_.emit(ev);
}

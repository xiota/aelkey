#include "device_input.h"

#include <climits>  // for PATH_MAX
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>

#include <fcntl.h>
#include <glob.h>
#include <libevdev/libevdev-uinput.h>
#include <libevdev/libevdev.h>
#include <libudev.h>
#include <libusb-1.0/libusb.h>
#include <linux/hidraw.h>
#include <sol/sol.hpp>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "aelkey_state.h"
#include "device_gatt.h"
#include "device_helpers.h"
#include "dispatcher_libusb.h"

// Parse a single InputDecl from a Lua table.
InputDecl parse_input(sol::table tbl) {
  InputDecl decl;

  // id
  if (sol::object v = tbl["id"]; v.valid() && v.is<std::string>()) {
    decl.id = v.as<std::string>();
  }

  // type
  if (sol::object v = tbl["type"]; v.valid() && v.is<std::string>()) {
    decl.type = v.as<std::string>();
  }

  // grab
  if (sol::object v = tbl["grab"]; v.valid() && v.is<bool>()) {
    decl.grab = v.as<bool>();
  }

  // vendor
  if (sol::object v = tbl["vendor"]; v.valid() && v.is<int>()) {
    decl.vendor = v.as<int>();
  }

  // product
  if (sol::object v = tbl["product"]; v.valid() && v.is<int>()) {
    decl.product = v.as<int>();
  }

  // bus
  if (sol::object v = tbl["bus"]; v.valid() && v.is<std::string>()) {
    std::string busstr = v.as<std::string>();
    if (busstr == "usb") {
      decl.bus = BUS_USB;
    } else if (busstr == "bluetooth") {
      decl.bus = BUS_BLUETOOTH;
    } else if (busstr == "pci") {
      decl.bus = BUS_PCI;
    }
  }

  // interface
  if (sol::object v = tbl["interface"]; v.valid() && v.is<int>()) {
    decl.interface = v.as<int>();
  }

  // name
  if (sol::object v = tbl["name"]; v.valid() && v.is<std::string>()) {
    decl.name = v.as<std::string>();
  }

  // phys
  if (sol::object v = tbl["phys"]; v.valid() && v.is<std::string>()) {
    decl.phys = v.as<std::string>();
  }

  // uniq
  if (sol::object v = tbl["uniq"]; v.valid() && v.is<std::string>()) {
    decl.uniq = v.as<std::string>();
  }

  // capabilities: array of { type = "EV_KEY", code = "KEY_A" }
  if (sol::object caps_obj = tbl["capabilities"];
      caps_obj.valid() && caps_obj.is<sol::table>()) {
    sol::table caps = caps_obj.as<sol::table>();
    caps.for_each([&](sol::object /*k*/, sol::object v) {
      if (!v.is<sol::table>()) {
        return;
      }
      sol::table cap_tbl = v.as<sol::table>();

      std::string type_str;
      std::string code_str;

      if (sol::object t = cap_tbl["type"]; t.valid() && t.is<std::string>()) {
        type_str = t.as<std::string>();
      }
      if (sol::object c = cap_tbl["code"]; c.valid() && c.is<std::string>()) {
        code_str = c.as<std::string>();
      }

      if (!type_str.empty() && !code_str.empty()) {
        int type_id = libevdev_event_type_from_name(type_str.c_str());
        int code_id = libevdev_event_code_from_name(type_id, code_str.c_str());
        if (type_id >= 0 && code_id >= 0) {
          decl.capabilities.emplace_back(type_id, code_id);
        }
      }
    });
  }

  // service
  if (sol::object v = tbl["service"]; v.valid() && v.is<int>()) {
    decl.service = v.as<int>();
  }

  // characteristic
  if (sol::object v = tbl["characteristic"]; v.valid() && v.is<int>()) {
    decl.characteristic = v.as<int>();
  }

  // on_event callback
  if (sol::object v = tbl["on_event"]; v.valid() && v.is<std::string>()) {
    decl.on_event = v.as<std::string>();
  }

  // on_state callback
  if (sol::object v = tbl["on_state"]; v.valid() && v.is<std::string>()) {
    decl.on_state = v.as<std::string>();
  }

  return decl;
}

static int get_interface_num(const std::string &devnode) {
  struct udev *udev = udev_new();
  if (!udev) {
    std::fprintf(stderr, "Failed to init udev\n");
    return -1;
  }

  // Create udev device object from the hidraw node
  struct udev_device *dev = udev_device_new_from_subsystem_sysname(
      udev, "hidraw", devnode.substr(devnode.find_last_of('/') + 1).c_str()
  );
  if (!dev) {
    udev_unref(udev);
    return -1;
  }

  const char *iface_str = udev_device_get_property_value(dev, "ID_USB_INTERFACE_NUM");
  int iface = -1;
  if (iface_str) {
    iface = std::stoi(iface_str, nullptr, 16);  // property is hex string like "01"
  }

  udev_device_unref(dev);
  udev_unref(udev);
  return iface;
}

static std::string enumerate_and_match(
    const char *subsystem,
    std::function<std::string(struct udev_device *)> matcher
) {
  auto &state = AelkeyState::instance();
  struct udev_enumerate *enumerate = udev_enumerate_new(state.g_udev);
  udev_enumerate_add_match_subsystem(enumerate, subsystem);
  udev_enumerate_scan_devices(enumerate);

  struct udev_list_entry *devices = udev_enumerate_get_list_entry(enumerate);
  struct udev_list_entry *entry;

  std::string result;
  udev_list_entry_foreach(entry, devices) {
    const char *path = udev_list_entry_get_name(entry);
    struct udev_device *dev = udev_device_new_from_syspath(state.g_udev, path);
    if (!dev) {
      continue;
    }

    result = matcher(dev);
    udev_device_unref(dev);
    if (!result.empty()) {
      break;
    }
  }

  udev_enumerate_unref(enumerate);
  return result;
}

std::string match_device(const InputDecl &decl) {
  if (decl.type == "hidraw") {
    return enumerate_and_match("hidraw", [&](struct udev_device *dev) {
      const char *devnode = udev_device_get_devnode(dev);
      if (!devnode) {
        return std::string{};
      }
      int fd = open(devnode, O_RDONLY | O_NONBLOCK);
      if (fd < 0) {
        return std::string{};
      }

      struct hidraw_devinfo info;
      if (ioctl(fd, HIDIOCGRAWINFO, &info) == 0) {
        bool match = true;
        if (decl.bus && static_cast<int>(info.bustype) != decl.bus) {
          match = false;
        }

        int vendor = static_cast<unsigned short>(info.vendor);
        int product = static_cast<unsigned short>(info.product);

        if (decl.vendor && vendor != decl.vendor) {
          match = false;
        }
        if (decl.product && product != decl.product) {
          match = false;
        }

        if (!decl.name.empty()) {
          char name[256] = { 0 };
          if (ioctl(fd, HIDIOCGRAWNAME(sizeof(name) - 1), name) >= 0) {
            std::string devname(name);
            if (!match_string(decl.name, devname)) {
              match = false;
            }
          } else {
            match = false;
          }
        }

        if (!decl.phys.empty()) {
          char phys[64] = { 0 };
          if (ioctl(fd, HIDIOCGRAWPHYS(sizeof(phys) - 1), phys) >= 0) {
            if (!match_string(decl.phys, phys)) {
              std::cout << std::string(phys) << std::endl;
              match = false;
            }
          }
        }

        if (!decl.uniq.empty()) {
          char uniq[64] = { 0 };
          if (ioctl(fd, HIDIOCGRAWUNIQ(sizeof(uniq) - 1), uniq) >= 0) {
            if (!match_string(decl.uniq, uniq)) {
              std::cout << std::string(uniq) << std::endl;
              match = false;
            }
          }
        }

        if (decl.interface >= 0) {
          int iface = get_interface_num(devnode);
          if (iface != decl.interface) {
            match = false;
          }
        }

        if (match) {
          std::cout << "Matched " << decl.id << " → " << devnode << std::endl;
          close(fd);
          return std::string{ devnode };
        }
      }

      close(fd);
      return std::string{};
    });
  } else if (decl.type == "evdev") {
    return enumerate_and_match("input", [&](struct udev_device *dev) {
      const char *devnode = udev_device_get_devnode(dev);
      if (!devnode) {
        return std::string{};
      }
      int fd = open(devnode, O_RDONLY | O_NONBLOCK);
      if (fd < 0) {
        return std::string{};
      }

      struct libevdev *evdev = nullptr;
      if (libevdev_new_from_fd(fd, &evdev) == 0) {
        bool match = true;
        if (decl.bus && libevdev_get_id_bustype(evdev) != decl.bus) {
          match = false;
        }
        if (decl.vendor && libevdev_get_id_vendor(evdev) != decl.vendor) {
          match = false;
        }
        if (decl.product && libevdev_get_id_product(evdev) != decl.product) {
          match = false;
        }
        if (!decl.name.empty() && !match_string(decl.name, (libevdev_get_name(evdev) ?: ""))) {
          match = false;
        }
        if (!decl.phys.empty() && !match_string(decl.phys, (libevdev_get_phys(evdev) ?: ""))) {
          match = false;
        }
        if (!decl.uniq.empty() && !match_string(decl.uniq, (libevdev_get_uniq(evdev) ?: ""))) {
          match = false;
        }

        for (auto &[type, code] : decl.capabilities) {
          if (!libevdev_has_event_code(evdev, type, code)) {
            match = false;
            break;
          }
        }

        if (match) {
          std::cout << "Matched " << decl.id << " → " << devnode << " ("
                    << (libevdev_get_name(evdev) ?: "") << ")\n";
          libevdev_free(evdev);
          close(fd);
          return std::string{ devnode };
        }
      }
      libevdev_free(evdev);
      close(fd);
      return std::string{};
    });
  } else if (decl.type == "libusb") {
    return enumerate_and_match("usb", [&](struct udev_device *dev) {
      const char *vid = udev_device_get_property_value(dev, "ID_VENDOR_ID");
      const char *pid = udev_device_get_property_value(dev, "ID_MODEL_ID");
      if (vid && pid && std::stoi(vid, nullptr, 16) == decl.vendor &&
          std::stoi(pid, nullptr, 16) == decl.product) {
        return std::string{ udev_device_get_syspath(dev) };
      }
      return std::string{};
    });
  } else if (decl.type == "gatt") {
    return match_gatt_device(decl);
  }

  return {};
}

bool try_evdev_grab(InputCtx &ctx) {
  if (!ctx.grab_pending || !ctx.idev) {
    return false;
  }

  // First: check kernel key bitmap via EVIOCGKEY
  unsigned long key_bits[(KEY_MAX + 1) / (sizeof(unsigned long) * 8)] = { 0 };
  if (ioctl(ctx.fd, EVIOCGKEY(sizeof(key_bits)), key_bits) >= 0) {
    for (int code = 0; code <= KEY_MAX; ++code) {
      if (key_bits[code / (sizeof(unsigned long) * 8)] &
          (1UL << (code % (sizeof(unsigned long) * 8)))) {
        return false;  // kernel thinks key is down
      }
    }
  }

  // Second: check libevdev's internal state
  for (int code = 0; code <= KEY_MAX; ++code) {
    int value = 0;
    if (libevdev_fetch_event_value(ctx.idev, EV_KEY, code, &value) == 0 && value == 1) {
      return false;  // libevdev thinks key is down
    }
  }

  // Attempt grab
  int rc = libevdev_grab(ctx.idev, LIBEVDEV_GRAB);
  if (rc < 0) {
    std::fprintf(
        stderr, "Deferred grab failed for %s: %s\n", ctx.decl.id.c_str(), strerror(-rc)
    );
    return false;
  }

  std::cout << "Grabbed device exclusively: " << ctx.decl.id << std::endl;
  ctx.grab_pending = false;
  ctx.grabbed = true;
  return true;
}

static InputCtx attach_device_helper(
    const std::string &devnode,
    const InputDecl &in,
    std::unordered_map<std::string, InputCtx> &input_map,
    std::unordered_map<std::string, std::vector<struct input_event>> &frames,
    int epfd
) {
  InputCtx ctx;
  ctx.decl = in;

  if (in.type == "hidraw") {
    // hidraw: open fd, no libevdev init
    ctx.fd = ::open(devnode.c_str(), O_RDWR | O_NONBLOCK);
    if (ctx.fd < 0) {
      perror("open");
      return ctx;  // ctx.fd == -1 signals failure
    }

    std::cout << "Attached hidraw: " << devnode << std::endl;
    ctx.active = true;

    if (in.grab) {
      int flags = fcntl(ctx.fd, F_GETFL, 0);
      if (flags != -1) {
        fcntl(ctx.fd, F_SETFL, flags & ~O_NONBLOCK);
      }
    }

    // register with epoll
    struct epoll_event evreg{};
    evreg.events = EPOLLIN;
    evreg.data.fd = ctx.fd;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, ctx.fd, &evreg) < 0) {
      perror("epoll_ctl EPOLL_CTL_ADD hidraw");
      close(ctx.fd);
      ctx.fd = -1;
      ctx.active = false;
    }
  } else if (in.type == "libusb") {
    auto &usb = DispatcherLibUSB::instance();
    ctx.usb_handle = usb.open_device(in.vendor, in.product);
    if (!ctx.usb_handle) {
      return ctx;
    }

    if (usb.claim_interface(ctx.usb_handle, in.interface) != 0) {
      libusb_close(ctx.usb_handle);
      ctx.usb_handle = nullptr;
      return ctx;
    }

    std::cout << "Attached libusb device: " << in.id << std::endl;
    ctx.active = true;

    // no ctx.fd, epoll integration handled by pollfd notifiers
  } else if (in.type == "gatt") {
    ensure_gatt_initialized();

    // devnode is the characteristic path discovered in match_device()
    InputDecl decl_copy = in;
    decl_copy.devnode = devnode;  // <- important

    ctx = attach_gatt_device(decl_copy);
  } else if (in.type == "evdev") {
    ctx.fd = ::open(devnode.c_str(), O_RDWR | O_NONBLOCK);
    if (ctx.fd < 0) {
      perror("open");
      return ctx;
    }

    struct libevdev *idev = nullptr;
    if (libevdev_new_from_fd(ctx.fd, &idev) < 0) {
      std::fprintf(stderr, "Failed to init libevdev for %s\n", devnode.c_str());
      close(ctx.fd);
      ctx.fd = -1;
      ctx.active = false;
      return ctx;
    }
    ctx.idev = idev;
    frames[in.id] = {};  // keyed by string id now

    if (libevdev_has_event_type(idev, EV_FF)) {
      ctx.haptics.supported = true;
      std::printf("Haptics: sink '%s' supports FF\n", in.id.c_str());
    }

    std::cout << "Attached evdev: " << libevdev_get_name(idev) << std::endl;
    ctx.active = true;

    if (in.grab) {
      ctx.grab_pending = true;
      try_evdev_grab(ctx);
    }

    // register with epoll
    struct epoll_event evreg{};
    evreg.events = EPOLLIN;
    evreg.data.fd = ctx.fd;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, ctx.fd, &evreg) < 0) {
      perror("epoll_ctl EPOLL_CTL_ADD evdev");
      libevdev_grab(ctx.idev, LIBEVDEV_UNGRAB);
      libevdev_free(ctx.idev);
      ctx.idev = nullptr;
      close(ctx.fd);
      ctx.fd = -1;
      ctx.active = false;
    }
  } else {
    std::fprintf(stderr, "Unknown input type: %s\n", in.type.c_str());
    return ctx;
  }

  return ctx;
}

void parse_inputs_from_lua(sol::this_state ts) {
  sol::state_view lua(ts);

  auto &state = AelkeyState::instance();
  state.input_decls.clear();

  sol::object obj = lua["inputs"];
  if (!obj.valid() || !obj.is<sol::table>()) {
    return;
  }

  sol::table inputs = obj.as<sol::table>();

  inputs.for_each([&](sol::object /*k*/, sol::object v) {
    if (v.is<sol::table>()) {
      InputDecl decl = parse_input(v.as<sol::table>());
      if (!decl.id.empty()) {
        state.input_decls.push_back(decl);
      }
    }
  });
}

bool attach_input_device(const std::string &devnode, const InputDecl &decl) {
  auto &state = AelkeyState::instance();

  // already attached?
  if (state.input_map.find(decl.id) != state.input_map.end()) {
    std::cout << "Device already attached: " << decl.id << std::endl;
    return false;
  }

  InputCtx ctx = attach_device_helper(devnode, decl, state.input_map, state.frames, state.epfd);

  // failure check
  if (!ctx.active) {
    std::fprintf(
        stderr,
        "Failed to attach input: %s (%s)\n",
        decl.id.c_str(),
        devnode.empty() ? decl.type.c_str() : devnode.c_str()
    );
    return false;
  }

  // store context keyed by string id
  state.input_map[decl.id] = std::move(ctx);
  return true;
}

InputDecl detach_input_device(const std::string &dev_id) {
  InputDecl decl{};

  auto &state = AelkeyState::instance();
  auto im = state.input_map.find(dev_id);
  if (im == state.input_map.end()) {
    return decl;  // nothing to detach
  }

  InputCtx &ctx = im->second;
  decl = ctx.decl;

  // Detach from gatt
  if (ctx.decl.type == "gatt") {
    detach_gatt_device(ctx);
  }

  // Remove from epoll if fd is valid
  if (state.epfd >= 0 && ctx.fd >= 0) {
    epoll_ctl(state.epfd, EPOLL_CTL_DEL, ctx.fd, nullptr);
  }

  // Free libevdev resources if present
  if (ctx.idev) {
    libevdev_grab(ctx.idev, LIBEVDEV_UNGRAB);
    libevdev_free(ctx.idev);
    ctx.idev = nullptr;
  }

  // Close fd if valid
  if (ctx.fd >= 0) {
    close(ctx.fd);
    ctx.fd = -1;
    ctx.active = false;
  }

  // Close libusb handle if present
  if (ctx.usb_handle) {
    // Cancel and free any active libusb transfers
    for (auto *t : ctx.transfers) {
      libusb_cancel_transfer(t);
    }
    ctx.transfers.clear();

    libusb_close(ctx.usb_handle);
    ctx.usb_handle = nullptr;
    ctx.active = false;
  }

  // Erase from maps keyed by string id
  state.input_map.erase(im);
  state.frames.erase(dev_id);

  return decl;
}

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
#include <lua.hpp>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "aelkey_state.h"
#include "device_libusb.h"

InputDecl parse_input(lua_State *L, int index) {
  InputDecl decl;
  lua_pushnil(L);
  while (lua_next(L, index)) {
    std::string key = lua_tostring(L, -2);

    if (key == "id" && lua_isstring(L, -1)) {
      decl.id = lua_tostring(L, -1);
    } else if (key == "type" && lua_isstring(L, -1)) {
      decl.type = lua_tostring(L, -1);
    } else if (key == "writable" && lua_isboolean(L, -1)) {
      decl.writable = lua_toboolean(L, -1);
    } else if (key == "grab" && lua_isboolean(L, -1)) {
      decl.grab = lua_toboolean(L, -1);
    } else if (key == "vendor" && lua_isnumber(L, -1)) {
      decl.vendor = (int)lua_tointeger(L, -1);
    } else if (key == "product" && lua_isnumber(L, -1)) {
      decl.product = (int)lua_tointeger(L, -1);
    } else if (key == "bus" && lua_isstring(L, -1)) {
      std::string busstr = lua_tostring(L, -1);
      if (busstr == "usb") {
        decl.bus = BUS_USB;
      } else if (busstr == "bluetooth") {
        decl.bus = BUS_BLUETOOTH;
      } else if (busstr == "pci") {
        decl.bus = BUS_PCI;
      }
    } else if (key == "interface" && lua_isnumber(L, -1)) {
      decl.interface = (int)lua_tointeger(L, -1);
    } else if (key == "name" && lua_isstring(L, -1)) {
      decl.name = lua_tostring(L, -1);
    } else if (key == "phys" && lua_isstring(L, -1)) {
      decl.phys = lua_tostring(L, -1);
    } else if (key == "uniq" && lua_isstring(L, -1)) {
      decl.uniq = lua_tostring(L, -1);
    } else if (key == "capabilities" && lua_istable(L, -1)) {
      int len = lua_objlen(L, -1);
      for (int i = 1; i <= len; i++) {
        lua_rawgeti(L, -1, i);
        if (lua_istable(L, -1)) {
          lua_getfield(L, -1, "type");
          std::string type = lua_tostring(L, -1);
          lua_pop(L, 1);

          lua_getfield(L, -1, "code");
          std::string code = lua_tostring(L, -1);
          lua_pop(L, 1);

          int type_id = libevdev_event_type_from_name(type.c_str());
          int code_id = libevdev_event_code_from_name(type_id, code.c_str());
          if (type_id >= 0 && code_id >= 0) {
            decl.capabilities.emplace_back(type_id, code_id);
          }
        }
        lua_pop(L, 1);
      }
    } else if (key == "callback_events" && lua_isstring(L, -1)) {
      decl.callback_events = lua_tostring(L, -1);
    } else if (key == "callback_state" && lua_isstring(L, -1)) {
      decl.callback_state = lua_tostring(L, -1);
    }

    lua_pop(L, 1);
  }
  return decl;
}

static int get_interface_num(const std::string &devnode) {
  struct udev *udev = udev_new();
  if (!udev) {
    std::cerr << "Failed to init udev\n";
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

static int ensure_claimed(libusb_device_handle *devh, const InputDecl &in) {
  int iface = (in.interface == -1) ? 0 : in.interface;

  if (libusb_kernel_driver_active(devh, iface) == 1) {
    int d = libusb_detach_kernel_driver(devh, iface);
    if (d != 0) {
      std::cerr << "Failed to detach kernel driver: " << libusb_error_name(d) << "\n";
      return d;
    }
  }

  int r = libusb_claim_interface(devh, iface);
  if (r != 0) {
    std::cerr << "Failed to claim interface " << iface << ": " << libusb_error_name(r) << "\n";
    return r;
  }

  return 0;
}

static std::string enumerate_and_match(
    const char *subsystem,
    std::function<std::string(struct udev_device *)> matcher
) {
  struct udev_enumerate *enumerate = udev_enumerate_new(aelkey_state.g_udev);
  udev_enumerate_add_match_subsystem(enumerate, subsystem);
  udev_enumerate_scan_devices(enumerate);

  struct udev_list_entry *devices = udev_enumerate_get_list_entry(enumerate);
  struct udev_list_entry *entry;

  std::string result;
  udev_list_entry_foreach(entry, devices) {
    const char *path = udev_list_entry_get_name(entry);
    struct udev_device *dev = udev_device_new_from_syspath(aelkey_state.g_udev, path);
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
  if (decl.type == "libusb") {
    return enumerate_and_match("usb", [&](struct udev_device *dev) {
      const char *vid = udev_device_get_property_value(dev, "ID_VENDOR_ID");
      const char *pid = udev_device_get_property_value(dev, "ID_MODEL_ID");
      if (vid && pid && std::stoi(vid, nullptr, 16) == decl.vendor &&
          std::stoi(pid, nullptr, 16) == decl.product) {
        return std::string{ udev_device_get_syspath(dev) };
      }
      return std::string{};
    });
  } else if (decl.type == "hidraw") {
    return enumerate_and_match("hidraw", [&](struct udev_device *dev) {
      const char *devnode = udev_device_get_devnode(dev);
      if (!devnode) {
        return std::string{};
      }
      // keep your existing ioctl checks here
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
            if (decl.name != devname) {
              match = false;
            }
          } else {
            match = false;
          }
        }

        if (!decl.phys.empty()) {
          char phys[64] = { 0 };
          if (ioctl(fd, HIDIOCGRAWPHYS(sizeof(phys) - 1), phys) >= 0) {
            if (decl.phys != std::string(phys)) {
              std::cout << std::string(phys) << std::endl;
              match = false;
            }
          }
        }

        if (!decl.uniq.empty()) {
          char uniq[64] = { 0 };
          if (ioctl(fd, HIDIOCGRAWUNIQ(sizeof(uniq) - 1), uniq) >= 0) {
            if (decl.uniq != std::string(uniq)) {
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
        }
      }
      close(fd);
      return std::string{ devnode };
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
        if (!decl.name.empty() && decl.name != (libevdev_get_name(evdev) ?: "")) {
          match = false;
        }
        if (!decl.phys.empty() && decl.phys != (libevdev_get_phys(evdev) ?: "")) {
          match = false;
        }
        if (!decl.uniq.empty() && decl.uniq != (libevdev_get_uniq(evdev) ?: "")) {
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
  }
  return {};
}

InputCtx attach_device(
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
    ctx.fd = ::open(devnode.c_str(), O_RDONLY | O_NONBLOCK);
    if (ctx.fd < 0) {
      perror("open");
      return ctx;  // ctx.fd == -1 signals failure
    }

    std::cout << "Attached hidraw: " << devnode << std::endl;

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
    }
  } else if (in.type == "libusb") {
    ensure_libusb_initialized();

    ctx.usb_handle =
        libusb_open_device_with_vid_pid(aelkey_state.g_libusb, in.vendor, in.product);
    if (!ctx.usb_handle) {
      std::cerr << "Failed to open libusb device " << std::hex << in.vendor << ":" << in.product
                << std::dec << "\n";
      return ctx;
    }
    std::cout << "Attached libusb device: " << in.id << std::endl;

    ensure_claimed(ctx.usb_handle, in);

    // no ctx.fd, epoll integration handled by pollfd notifiers
  } else {
    // default: evdev
    ctx.fd = ::open(devnode.c_str(), O_RDONLY | O_NONBLOCK);
    if (ctx.fd < 0) {
      perror("open");
      return ctx;
    }

    struct libevdev *idev = nullptr;
    if (libevdev_new_from_fd(ctx.fd, &idev) < 0) {
      std::cerr << "Failed to init libevdev for " << devnode << std::endl;
      close(ctx.fd);
      ctx.fd = -1;
      return ctx;
    }
    ctx.idev = idev;
    frames[in.id] = {};  // keyed by string id now
    std::cout << "Attached evdev: " << libevdev_get_name(idev) << std::endl;

    if (in.grab) {
      int rc = libevdev_grab(idev, LIBEVDEV_GRAB);
      if (rc < 0) {
        std::cerr << "Failed to grab device " << devnode << ": " << strerror(-rc) << std::endl;
      } else {
        std::cout << "Grabbed device exclusively: " << devnode << std::endl;
      }
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
    }
  }

  // Save context keyed by string id if valid
  if (!in.id.empty() && (ctx.fd >= 0 || ctx.usb_handle)) {
    input_map[in.id] = ctx;
  }

  return ctx;
}

void parse_inputs_from_lua(lua_State *L) {
  aelkey_state.input_decls.clear();

  lua_getglobal(L, "inputs");
  if (!lua_istable(L, -1)) {
    lua_pop(L, 1);
    return;
  }

  lua_pushnil(L);
  while (lua_next(L, -2) != 0) {
    if (lua_istable(L, -1)) {
      InputDecl decl = parse_input(L, lua_gettop(L));
      if (!decl.id.empty()) {
        aelkey_state.input_decls.push_back(decl);
      }
    }
    lua_pop(L, 1);  // pop value, keep key
  }
  lua_pop(L, 1);  // pop inputs table
}

bool attach_input_device(const std::string &devnode, const InputDecl &decl) {
  // already attached?
  if (aelkey_state.input_map.find(decl.id) != aelkey_state.input_map.end()) {
    std::cout << "Device already attached: " << decl.id << std::endl;
    return false;
  }

  InputCtx ctx = attach_device(
      devnode, decl, aelkey_state.input_map, aelkey_state.frames, aelkey_state.epfd
  );

  // failure check: neither fd nor usb_handle valid
  if (ctx.fd < 0 && !ctx.usb_handle) {
    std::cerr << "Failed to attach input: " << decl.id << " ("
              << (devnode.empty() ? decl.type : devnode) << ")" << std::endl;
    return false;
  }

  // store context keyed by string id
  aelkey_state.input_map[decl.id] = std::move(ctx);
  return true;
}

InputDecl detach_input_device(const std::string &dev_id) {
  InputDecl decl{};

  auto im = aelkey_state.input_map.find(dev_id);
  if (im == aelkey_state.input_map.end()) {
    return decl;  // nothing to detach
  }

  InputCtx &ctx = im->second;
  decl = ctx.decl;

  // Remove from epoll if fd is valid
  if (aelkey_state.epfd >= 0 && ctx.fd >= 0) {
    epoll_ctl(aelkey_state.epfd, EPOLL_CTL_DEL, ctx.fd, nullptr);
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
  }

  // Close libusb handle if present
  if (ctx.usb_handle) {
    // Cancel and free any active libusb transfers
    for (auto *t : ctx.transfers) {
      libusb_cancel_transfer(t);
      libusb_free_transfer(t);
    }
    ctx.transfers.clear();

    libusb_close(ctx.usb_handle);
    ctx.usb_handle = nullptr;
  }

  // Erase from maps keyed by string id
  aelkey_state.input_map.erase(im);
  aelkey_state.frames.erase(dev_id);

  return decl;
}

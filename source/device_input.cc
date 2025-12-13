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

std::string match_device(const InputDecl &decl) {
  if (decl.type == "hidraw") {
    glob_t g;
    if (glob("/dev/hidraw*", 0, nullptr, &g) == 0) {
      for (size_t i = 0; i < g.gl_pathc; i++) {
        std::string devnode = g.gl_pathv[i];
        int fd = open(devnode.c_str(), O_RDONLY | O_NONBLOCK);
        if (fd < 0) {
          continue;
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
            close(fd);
            globfree(&g);
            return devnode;
          }
        }
        close(fd);
      }
    }
    globfree(&g);
    return {};
  } else {
    glob_t g;
    if (glob("/dev/input/event*", 0, nullptr, &g) == 0) {
      for (size_t i = 0; i < g.gl_pathc; i++) {
        std::string devnode = g.gl_pathv[i];
        int fd = open(devnode.c_str(), O_RDONLY | O_NONBLOCK);
        if (fd < 0) {
          continue;
        }

        struct libevdev *dev = nullptr;
        if (libevdev_new_from_fd(fd, &dev) == 0) {
          bool match = true;
          if (decl.bus && libevdev_get_id_bustype(dev) != decl.bus) {
            match = false;
          }
          if (decl.vendor && libevdev_get_id_vendor(dev) != decl.vendor) {
            match = false;
          }
          if (decl.product && libevdev_get_id_product(dev) != decl.product) {
            match = false;
          }
          if (!decl.name.empty() && decl.name != (libevdev_get_name(dev) ?: "")) {
            match = false;
          }
          if (!decl.phys.empty() && decl.phys != (libevdev_get_phys(dev) ?: "")) {
            match = false;
          }
          if (!decl.uniq.empty() && decl.uniq != (libevdev_get_uniq(dev) ?: "")) {
            match = false;
          }

          for (auto &[type, code] : decl.capabilities) {
            if (!libevdev_has_event_code(dev, type, code)) {
              match = false;
              break;
            }
          }

          if (match) {
            std::cout << "Matched " << decl.id << " → " << devnode << " ("
                      << (libevdev_get_name(dev) ?: "") << ")\n";
            libevdev_free(dev);
            close(fd);
            globfree(&g);
            return devnode;
          }
        }
        libevdev_free(dev);
        close(fd);
      }
    }
    globfree(&g);
    return {};
  }
}

int attach_device(
    const std::string &devnode,
    const InputDecl &in,
    std::unordered_map<int, InputCtx> &input_map,
    std::unordered_map<int, std::vector<struct input_event>> &frames,
    int epfd
) {
  // Open the device node
  int fd = open(devnode.c_str(), O_RDONLY | O_NONBLOCK);
  if (fd < 0) {
    perror("open");
    return -1;
  }

  InputCtx ctx;
  ctx.decl = in;

  if (in.type == "hidraw") {
    // hidraw: no libevdev init
    std::cout << "Attached hidraw: " << devnode << std::endl;

    if (in.grab) {
      int flags = fcntl(fd, F_GETFL, 0);
      if (flags != -1) {
        fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
      }
    }
  } else if (in.type == "libusb") {
    if (!aelkey_state.g_libusb) {
      if (libusb_init(&aelkey_state.g_libusb) != 0) {
        std::cerr << "Failed to init libusb\n";
        return -1;
      }
    }
    ctx.usb_handle =
        libusb_open_device_with_vid_pid(aelkey_state.g_libusb, in.vendor, in.product);
    if (!ctx.usb_handle) {
      std::cerr << "Failed to open libusb device " << std::hex << in.vendor << ":" << in.product
                << std::dec << "\n";
      return -1;
    }
    std::cout << "Attached libusb device: " << in.id << std::endl;

    const struct libusb_pollfd **pfds = libusb_get_pollfds(aelkey_state.g_libusb);
    if (pfds) {
      for (int i = 0; pfds[i] != nullptr; i++) {
        struct epoll_event evreg{};
        evreg.events = pfds[i]->events;
        evreg.data.fd = pfds[i]->fd;
        if (epoll_ctl(epfd, EPOLL_CTL_ADD, pfds[i]->fd, &evreg) < 0) {
          if (errno != EEXIST) {
            perror("epoll_ctl add libusb fd");
          }
        }
      }
      free(pfds);
    }
    return 0;  // no usable fds for libusb
  } else {
    // default: evdev
    struct libevdev *idev = nullptr;
    if (libevdev_new_from_fd(fd, &idev) < 0) {
      std::cerr << "Failed to init libevdev for " << devnode << std::endl;
      close(fd);
      return -1;
    }
    ctx.idev = idev;
    aelkey_state.frames[fd] = {};
    std::cout << "Attached evdev: " << libevdev_get_name(idev) << std::endl;

    if (in.grab) {
      int rc = libevdev_grab(idev, LIBEVDEV_GRAB);
      if (rc < 0) {
        std::cerr << "Failed to grab device " << devnode << ": " << strerror(-rc) << std::endl;
      } else {
        std::cout << "Grabbed device exclusively: " << devnode << std::endl;
      }
    }
  }

  aelkey_state.input_map[fd] = ctx;

  struct epoll_event evreg{};
  evreg.events = EPOLLIN;
  evreg.data.fd = fd;
  if (epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &evreg) < 0) {
    perror("epoll_ctl EPOLL_CTL_ADD");
    if (ctx.idev) {
      libevdev_grab(ctx.idev, LIBEVDEV_UNGRAB);
      libevdev_free(ctx.idev);
    }
    aelkey_state.input_map.erase(fd);
    aelkey_state.frames.erase(fd);
    close(fd);
    return -1;
  }

  return fd;
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

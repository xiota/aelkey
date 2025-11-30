#include "device_input.h"

#include <cstring>
#include <iostream>

#include <fcntl.h>
#include <libevdev/libevdev-uinput.h>
#include <libevdev/libevdev.h>
#include <sys/epoll.h>
#include <unistd.h>

InputDecl parse_input(lua_State *L, int index) {
  InputDecl decl;
  lua_pushnil(L);
  while (lua_next(L, index)) {
    std::string key = lua_tostring(L, -2);

    if (key == "id" && lua_isstring(L, -1)) {
      decl.id = lua_tostring(L, -1);
    } else if (key == "kind" && lua_isstring(L, -1)) {
      decl.kind = lua_tostring(L, -1);
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
    } else if (key == "callback" && lua_isstring(L, -1)) {
      decl.callback = lua_tostring(L, -1);
    }

    lua_pop(L, 1);
  }
  return decl;
}

OutputDecl parse_output(lua_State *L, int index) {
  OutputDecl decl;
  lua_pushnil(L);
  while (lua_next(L, index)) {
    std::string key = lua_tostring(L, -2);
    if (key == "id" && lua_isstring(L, -1)) {
      decl.id = lua_tostring(L, -1);
    } else if (key == "type" && lua_isstring(L, -1)) {
      decl.type = lua_tostring(L, -1);
    } else if (key == "name" && lua_isstring(L, -1)) {
      decl.name = lua_tostring(L, -1);
    }
    lua_pop(L, 1);
  }
  return decl;
}

std::string match_device(const InputDecl &decl) {
  for (int i = 0;; i++) {
    std::string devnode = "/dev/input/event" + std::to_string(i);
    int fd = open(devnode.c_str(), O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
      break;
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

      // Capability check: all must be present
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
        return devnode;
      }
    }
    libevdev_free(dev);
    close(fd);
  }
  return {};
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

  // Initialize libevdev
  struct libevdev *idev = nullptr;
  if (libevdev_new_from_fd(fd, &idev) < 0) {
    std::cerr << "Failed to init libevdev for " << devnode << std::endl;
    close(fd);
    return -1;
  }

  std::cout << "Attached input device: " << libevdev_get_name(idev) << std::endl;

  if (in.grab) {
    int rc = libevdev_grab(idev, LIBEVDEV_GRAB);
    if (rc < 0) {
      std::cerr << "Failed to grab device " << devnode << ": " << strerror(-rc) << std::endl;
    } else {
      std::cout << "Grabbed device exclusively: " << devnode << std::endl;
    }
  }

  // Populate InputCtx
  InputCtx ctx;
  ctx.decl = in;
  ctx.idev = idev;
  input_map[fd] = ctx;
  frames[fd] = {};

  struct epoll_event evreg{};
  evreg.events = EPOLLIN;
  evreg.data.fd = fd;
  if (epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &evreg) < 0) {
    perror("epoll_ctl EPOLL_CTL_ADD");
    libevdev_grab(idev, LIBEVDEV_UNGRAB);
    libevdev_free(idev);
    input_map.erase(fd);
    frames.erase(fd);
    close(fd);
    return -1;
  }

  return fd;
}

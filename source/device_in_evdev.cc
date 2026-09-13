#include "device_in_evdev.h"

#include <fcntl.h>
#include <libevdev/libevdev.h>
#include <libudev.h>
#include <unistd.h>

#include "aelkey_state.h"
#include "backend_udev.h"
#include "dispatcher_vulgate.h"
#include "manager_device_in.h"
#include "manager_haptics.h"
#include "utils/regex_match.h"
#include "utils/signal.h"

DeviceInEvdev::~DeviceInEvdev() {
  for (auto &[fd, st] : devs_) {
    if (st.idev) {
      libevdev_grab(st.idev, LIBEVDEV_UNGRAB);
      libevdev_free(st.idev);
    }
    close(fd);
  }
  devs_.clear();
  device_ids_.clear();
}

DeviceInEvdev::DeviceInEvdev() {
  tok_udev_event_ =
      BackendUdev::instance().sig_udev_event_.subscribe([this](const UdevEvent &ev) {
        if (ev.subsystem != "input") {
          return;
        }

        // Handle add/remove
        if (ev.action == "add") {
          // Try to match and attach
          for (auto &decl : AelkeyState::instance().input_decls) {
            if (decl.type != "evdev") {
              continue;
            }

            std::string matched;
            if (match(decl, matched) && matched == ev.devnode) {
              if (ManagerDeviceIn::instance().attach(matched, decl)) {
                break;
              }
            }
          }
        } else if (ev.action == "remove") {
          // Try to detach
          for (auto &decl : AelkeyState::instance().input_decls) {
            if (decl.type != "evdev") {
              continue;
            }
            if (decl.devnode == ev.devnode) {
              if (ManagerDeviceIn::instance().detach(decl.id)) {
                break;
              }
            }
          }
        }
      });
}

bool DeviceInEvdev::match(InputDecl &decl, std::string &devnode_out) {
  if (decl.type != "evdev") {
    return false;
  }

  std::string result = BackendUdev::instance().enumerate_and_match(
      "input", [&](struct udev_device *dev) -> std::string {
        const char *devnode = udev_device_get_devnode(dev);
        if (!devnode) {
          return {};
        }

        int fd = open(devnode, O_RDONLY | O_NONBLOCK);
        if (fd < 0) {
          return {};
        }

        struct libevdev *evdev = nullptr;
        bool ok = false;

        if (libevdev_new_from_fd(fd, &evdev) == 0) {
          ok = true;

          int dev_vendor = libevdev_get_id_vendor(evdev);
          int dev_product = libevdev_get_id_product(evdev);
          int dev_version = libevdev_get_id_version(evdev);

          // vid_pid matching
          bool vidpid_ok = decl.vid_pid.empty();
          for (auto &[v, p] : decl.vid_pid) {
            bool vendor_ok = (v == 0 || v == dev_vendor);
            bool product_ok = (p == 0 || p == dev_product);
            if (vendor_ok && product_ok) {
              vidpid_ok = true;
              break;
            }
          }
          if (!vidpid_ok) {
            ok = false;
          }

          if (decl.version != 0 && decl.version != dev_version) {
            ok = false;
          }

          if (decl.bus && libevdev_get_id_bustype(evdev) != decl.bus) {
            ok = false;
          }

          if (!decl.name.empty()) {
            const char *name = libevdev_get_name(evdev);
            if (!AelkeyUtil::match_string(decl.name, name ? name : "")) {
              ok = false;
            }
          }

          if (!decl.phys.empty()) {
            const char *phys = libevdev_get_phys(evdev);
            if (!AelkeyUtil::match_string(decl.phys, phys ? phys : "")) {
              ok = false;
            }
          }

          if (!decl.uniq.empty()) {
            const char *uniq = libevdev_get_uniq(evdev);
            if (!AelkeyUtil::match_string(decl.uniq, uniq ? uniq : "")) {
              ok = false;
            }
          }

          for (auto &[type, code] : decl.capabilities) {
            if (!libevdev_has_event_code(evdev, type, code)) {
              ok = false;
              break;
            }
          }

          for (int prop : decl.properties) {
            if (!libevdev_has_property(evdev, prop)) {
              ok = false;
              break;
            }
          }

          // store vendor, product, version after match
          if (ok) {
            decl.vendor = dev_vendor;
            decl.product = dev_product;
            decl.version = dev_version;
          }
        }

        libevdev_free(evdev);
        close(fd);

        return ok ? std::string(devnode) : std::string{};
      }
  );

  if (!result.empty()) {
    devnode_out = result;
    return true;
  }

  return false;
}

bool DeviceInEvdev::attach(const std::string &devnode, InputDecl &decl) {
  int fd = open(devnode.c_str(), O_RDWR | O_NONBLOCK);
  if (fd < 0) {
    perror("open evdev");
    return false;
  }

  struct libevdev *idev = nullptr;
  if (libevdev_new_from_fd(fd, &idev) < 0) {
    std::fprintf(stderr, "Failed to init libevdev for %s\n", devnode.c_str());
    close(fd);
    return false;
  }

  if (libevdev_has_event_type(idev, EV_FF)) {
    ManagerHaptics::instance().register_sink(decl.id, fd);
  }

  std::printf("Attached evdev: %s\n", libevdev_get_name(idev));

  EvdevDeviceState st;
  st.id = decl.id;
  st.idev = idev;
  st.grab_needed = decl.grab;
  devs_[fd] = std::move(st);
  device_ids_[decl.id] = fd;

  if (decl.grab) {
    try_evdev_grab(fd, decl);
  }

  DispatcherCb cb;
  cb.native = [this, fd, decl]() { handle_evdev_event(fd, decl); };
  cb.cleanup = [this](int fd_to_clean) {
    auto it = devs_.find(fd_to_clean);
    if (it != devs_.end()) {
      if (it->second.idev) {
        libevdev_grab(it->second.idev, LIBEVDEV_UNGRAB);
        libevdev_free(it->second.idev);
      }
      device_ids_.erase(it->second.id);
      devs_.erase(it);
    }
    close(fd_to_clean);
  };

  DispatcherVulgate::instance().register_device_fd(
      fd, EPOLLIN | EPOLLHUP | EPOLLERR, std::move(cb), decl.id
  );

  decl.devnode = devnode;
  decl.fd = fd;
  return true;
}

bool DeviceInEvdev::detach(const std::string &id) {
  auto it = device_ids_.find(id);
  if (it == device_ids_.end()) {
    return false;
  }

  int fd = it->second;
  DispatcherVulgate::instance().unregister_device_fd(fd);
  return true;
}

void DeviceInEvdev::handle_evdev_event(int fd, const InputDecl &decl) {
  auto it = devs_.find(fd);
  if (it == devs_.end()) {
    return;
  }

  auto &st = it->second;
  libevdev *idev = st.idev;
  auto &frame = st.frame;

  auto &state = AelkeyState::instance();
  sol::state_view lua(state.lua_vm);

  struct input_event ev;
  while (true) {
    int rc = libevdev_next_event(idev, LIBEVDEV_READ_FLAG_NORMAL, &ev);
    if (rc == 0) {
      frame.push_back(ev);

      if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
        if (!decl.on_event.empty()) {
          sol::object obj = lua[decl.on_event];
          if (obj.is<sol::function>()) {
            sol::function cb = obj.as<sol::function>();

            sol::table events_tbl = lua.create_table();
            int idx = 1;
            for (const auto &e : frame) {
              sol::table evt = lua.create_table();

              evt["device"] = decl.id;

              const char *tname = libevdev_event_type_get_name(e.type);
              const char *cname = libevdev_event_code_get_name(e.type, e.code);

              evt["type"] = tname ? tname : "";
              evt["code"] = cname ? cname : "";
              evt["value"] = e.value;
              evt["sec"] = static_cast<int>(e.time.tv_sec);
              evt["usec"] = static_cast<int>(e.time.tv_usec);

              events_tbl[idx++] = evt;
            }

            sol::protected_function pf = cb;
            sol::protected_function_result res = pf(events_tbl);
            if (!res.valid()) {
              sol::error err = res;
              std::fprintf(stderr, "Lua event callback error: %s\n", err.what());
            }
          }
        }
        frame.clear();
      }
    } else if (rc == -EAGAIN || rc == LIBEVDEV_READ_STATUS_SYNC) {
      break;
    } else {
      break;
    }
  }
}

bool DeviceInEvdev::try_evdev_grab(int fd, const InputDecl &decl) {
  auto it = devs_.find(fd);
  if (it == devs_.end()) {
    return false;
  }

  auto &st = it->second;
  if (!st.grab_needed) {
    return false;
  }

  libevdev *idev = st.idev;

  unsigned long key_bits[(KEY_MAX + 1) / (sizeof(unsigned long) * 8)] = { 0 };
  if (ioctl(fd, EVIOCGKEY(sizeof(key_bits)), key_bits) >= 0) {
    for (int code = 0; code <= KEY_MAX; ++code) {
      if (key_bits[code / (sizeof(unsigned long) * 8)] &
          (1UL << (code % (sizeof(unsigned long) * 8)))) {
        return false;
      }
    }
  }

  for (int code = 0; code <= KEY_MAX; ++code) {
    int value = 0;
    if (libevdev_fetch_event_value(idev, EV_KEY, code, &value) == 0 && value == 1) {
      return false;
    }
  }

  int rc = libevdev_grab(idev, LIBEVDEV_GRAB);
  if (rc < 0) {
    return false;
  }

  st.grab_needed = false;
  return true;
}

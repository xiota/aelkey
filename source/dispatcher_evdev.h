#pragma once

#include <iostream>
#include <map>

#include <fcntl.h>
#include <libevdev/libevdev.h>
#include <linux/input.h>
#include <sys/epoll.h>
#include <unistd.h>

#include "aelkey_state.h"
#include "device_declarations.h"
#include "device_in_manager.h"
#include "dispatcher.h"
#include "dispatcher_haptics.h"
#include "singleton.h"

struct EvdevDeviceState {
  std::string id;                  // stable device identifier
  libevdev *idev = nullptr;        // libevdev handle
  std::vector<input_event> frame;  // event batching buffer
  bool grab_needed = false;        // retry flag
};

class DispatcherEvdev : public Dispatcher<DispatcherEvdev> {
  friend class Singleton<DispatcherEvdev>;
  friend class Dispatcher<DispatcherEvdev>;

 protected:
  DispatcherEvdev() = default;
  ~DispatcherEvdev() = default;

 public:
  const char *type() const override {
    return "evdev";
  }

  void on_unregister(int fd) override {
    close(fd);
  }

  bool open_device(const std::string &devnode, InputDecl &decl) {
    // Open evdev node
    decl.fd = open(devnode.c_str(), O_RDWR | O_NONBLOCK);
    if (decl.fd < 0) {
      perror("open evdev");
      return false;
    }

    // Initialize libevdev
    struct libevdev *idev = nullptr;
    if (libevdev_new_from_fd(decl.fd, &idev) < 0) {
      std::fprintf(stderr, "Failed to init libevdev for %s\n", devnode.c_str());
      close(decl.fd);
      decl.fd = -1;
      return false;
    }

    // Detect FF support
    if (libevdev_has_event_type(idev, EV_FF)) {
      DispatcherHaptics::instance().register_sink(decl.id, decl.fd);
    }

    std::cout << "Attached evdev: " << libevdev_get_name(idev) << std::endl;

    // Register FD with epoll
    register_fd(decl.fd, EPOLLIN | EPOLLHUP | EPOLLERR);

    // Create device state entry
    EvdevDeviceState st;
    st.id = decl.id;
    st.idev = idev;
    st.grab_needed = decl.grab;
    devs_[decl.fd] = std::move(st);

    // Attempt grab after state is installed
    if (decl.grab) {
      try_evdev_grab(decl);
    }

    return true;
  }

  void close_device(InputDecl &decl) {
    // Unregister from epoll
    if (decl.fd >= 0) {
      unregister_fd(decl.fd);
    }

    // Free libevdev
    auto it = devs_.find(decl.fd);
    if (it != devs_.end()) {
      if (it->second.idev) {
        libevdev_grab(it->second.idev, LIBEVDEV_UNGRAB);
        libevdev_free(it->second.idev);
      }
      devs_.erase(it);
    }
  }

  // EPOLL callback
  void handle_event(EpollPayload *payload, uint32_t events) override {
    int fd = payload->fd;

    auto it = devs_.find(fd);
    if (it == devs_.end()) {
      return;
    }
    auto &st = it->second;
    const std::string &id = st.id;

    auto &state = AelkeyState::instance();
    auto decl_it = state.input_map.find(id);
    if (decl_it == state.input_map.end()) {
      return;  // device already detached
    }
    InputDecl &decl = decl_it->second;

    // HUP/ERR → detach device
    if (events & (EPOLLHUP | EPOLLERR)) {
      DeviceInManager::instance().detach(decl.id);
      return;
    }

    if (!(events & EPOLLIN)) {
      return;
    }

    dispatch_evdev_logic(decl);
  }

 private:
  void dispatch_evdev_logic(InputDecl &decl) {
    auto it = devs_.find(decl.fd);
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
      } else if (rc == -EAGAIN) {
        break;
      } else if (rc == LIBEVDEV_READ_STATUS_SYNC) {
        break;
      } else {
        break;
      }
    }
  }

  bool try_evdev_grab(InputDecl &decl) {
    auto it = devs_.find(decl.fd);
    if (it == devs_.end()) {
      return false;
    }

    auto &st = it->second;
    if (!st.grab_needed) {
      return false;
    }

    libevdev *idev = st.idev;

    // check kernel key bitmap via EVIOCGKEY
    unsigned long key_bits[(KEY_MAX + 1) / (sizeof(unsigned long) * 8)] = { 0 };
    if (ioctl(decl.fd, EVIOCGKEY(sizeof(key_bits)), key_bits) >= 0) {
      for (int code = 0; code <= KEY_MAX; ++code) {
        if (key_bits[code / (sizeof(unsigned long) * 8)] &
            (1UL << (code % (sizeof(unsigned long) * 8)))) {
          return false;  // kernel thinks key is down
        }
      }
    }

    // check libevdev's internal state
    for (int code = 0; code <= KEY_MAX; ++code) {
      int value = 0;
      if (libevdev_fetch_event_value(idev, EV_KEY, code, &value) == 0 && value == 1) {
        return false;  // libevdev thinks key is down
      }
    }

    // Attempt grab
    int rc = libevdev_grab(idev, LIBEVDEV_GRAB);
    if (rc < 0) {
      return false;
    }

    st.grab_needed = false;
    return true;
  }

  // fd → per-device state
  std::map<int, EvdevDeviceState> devs_;
};

template class Dispatcher<DispatcherEvdev>;

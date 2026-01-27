#pragma once

#include <iostream>
#include <unordered_map>

#include <fcntl.h>
#include <libevdev/libevdev.h>
#include <linux/input.h>
#include <unistd.h>

#include "aelkey_state.h"
#include "device_input.h"
#include "device_manager.h"
#include "dispatcher.h"
#include "dispatcher_registry.h"
#include "dispatcher_udev.h"
#include "singleton.h"

class DispatcherEvdev : public Dispatcher<DispatcherEvdev> {
  friend class Singleton<DispatcherEvdev>;

 protected:
  DispatcherEvdev() = default;
  ~DispatcherEvdev() = default;

 public:
  const char *type() const override {
    return "evdev";
  }

  bool open_device(const std::string &devnode, InputCtx &ctx) {
    // Open evdev node
    ctx.fd = open(devnode.c_str(), O_RDWR | O_NONBLOCK);
    if (ctx.fd < 0) {
      perror("open evdev");
      return false;
    }

    // Initialize libevdev
    struct libevdev *idev = nullptr;
    if (libevdev_new_from_fd(ctx.fd, &idev) < 0) {
      std::fprintf(stderr, "Failed to init libevdev for %s\n", devnode.c_str());
      close(ctx.fd);
      ctx.fd = -1;
      return false;
    }
    idev_map_[ctx.fd] = idev;

    // Initialize frame buffer
    auto &state = AelkeyState::instance();
    state.frames[ctx.decl.id] = {};

    // Detect FF support
    if (libevdev_has_event_type(idev, EV_FF)) {
      ctx.haptics.supported = true;
      std::printf("Haptics: sink '%s' supports FF\n", ctx.decl.id.c_str());
    }

    std::cout << "Attached evdev: " << libevdev_get_name(idev) << std::endl;

    // Initial grab attempt
    if (ctx.decl.grab) {
      grab_needed_[ctx.fd] = true;
      try_evdev_grab(ctx);
    }

    // Register FD with epoll
    register_fd(ctx.fd, EPOLLIN | EPOLLHUP | EPOLLERR);

    // store stable device ID
    devices_[ctx.fd] = ctx.decl.id;

    return true;
  }

  void close_device(InputCtx &ctx) {
    // Unregister from epoll
    if (ctx.fd >= 0) {
      unregister_fd(ctx.fd);
      devices_.erase(ctx.fd);
      grab_needed_.erase(ctx.fd);
    }

    // Free libevdev
    with_idev(ctx.fd, [&](libevdev *idev) {
      libevdev_grab(idev, LIBEVDEV_UNGRAB);
      libevdev_free(idev);
    });
    idev_map_.erase(ctx.fd);

    // Close FD
    if (ctx.fd >= 0) {
      close(ctx.fd);
      ctx.fd = -1;
    }
  }

  // EPOLL callback
  void handle_event(EpollPayload *payload, uint32_t events) override {
    int fd = payload->fd;

    auto it = devices_.find(fd);
    if (it == devices_.end()) {
      return;
    }

    const std::string &id = it->second;
    auto &state = AelkeyState::instance();

    auto ctx_it = state.input_map.find(id);
    if (ctx_it == state.input_map.end()) {
      return;  // device already detached
    }

    InputCtx &ctx = ctx_it->second;

    // HUP/ERR → detach device
    if (events & (EPOLLHUP | EPOLLERR)) {
      auto removed = DeviceManager::instance().detach(ctx.decl.id);
      if (removed && !removed->id.empty()) {
        DispatcherUdev::instance().notify_state_change(*removed, "remove");
      }
      return;
    }

    if (!(events & EPOLLIN)) {
      return;
    }

    dispatch_evdev_logic(ctx);
  }

 private:
  // --- libevdev helpers ---
  // Retrieve libevdev* for a given fd, or nullptr if not found.
  libevdev *get_idev(int fd) {
    auto it = idev_map_.find(fd);
    return (it != idev_map_.end()) ? it->second : nullptr;
  }

  // Call a lambda with libevdev* if it exists.
  // Usage: with_idev(fd, [&](libevdev* idev) { ... });
  template <typename F>
  void with_idev(int fd, F &&fn) {
    auto it = idev_map_.find(fd);
    if (it != idev_map_.end()) {
      fn(it->second);
    }
  }

  // Check if an idev exists for this fd.
  bool idev_exists(int fd) const {
    return idev_map_.count(fd) > 0;
  }

  void dispatch_evdev_logic(InputCtx &ctx) {
    libevdev *idev = get_idev(ctx.fd);
    if (!idev) {
      return;
    }

    auto &state = AelkeyState::instance();
    auto fit = state.frames.find(ctx.decl.id);
    if (fit == state.frames.end()) {
      return;
    }
    auto &frame = fit->second;

    sol::state_view lua(state.lua_vm);

    struct input_event ev;
    while (true) {
      int rc = libevdev_next_event(idev, LIBEVDEV_READ_FLAG_NORMAL, &ev);
      if (rc == 0) {
        frame.push_back(ev);

        if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
          if (!ctx.decl.on_event.empty()) {
            sol::object obj = lua[ctx.decl.on_event];
            if (obj.is<sol::function>()) {
              sol::function cb = obj.as<sol::function>();

              sol::table events_tbl = lua.create_table();
              int idx = 1;
              for (const auto &e : frame) {
                sol::table evt = lua.create_table();

                evt["device"] = ctx.decl.id;

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

  bool try_evdev_grab(InputCtx &ctx) {
    auto it = grab_needed_.find(ctx.fd);
    if (it == grab_needed_.end() || !it->second) {
      return false;
    }

    libevdev *idev = get_idev(ctx.fd);
    if (!idev) {
      return false;
    }

    // check kernel key bitmap via EVIOCGKEY
    unsigned long key_bits[(KEY_MAX + 1) / (sizeof(unsigned long) * 8)] = { 0 };
    if (ioctl(ctx.fd, EVIOCGKEY(sizeof(key_bits)), key_bits) >= 0) {
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

    grab_needed_[ctx.fd] = false;
    return true;
  }

  // fd → device id (stable)
  std::unordered_map<int, std::string> devices_;

  // fd → libevdev*
  std::unordered_map<int, libevdev *> idev_map_;

  // fd → flag
  std::unordered_map<int, bool> grab_needed_;
};

template class Dispatcher<DispatcherEvdev>;

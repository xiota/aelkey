#pragma once

#include <unordered_map>

#include <libevdev/libevdev.h>
#include <linux/input.h>

#include "aelkey_state.h"
#include "device_input.h"
#include "dispatcher.h"
#include "dispatcher_registry.h"
#include "dispatcher_udev.h"

class DispatcherEvdev : public Dispatcher<DispatcherEvdev> {
 public:
  // Called by attach_input_device() after creating InputCtx
  bool open_device(InputCtx &ctx, const std::string &devnode) {
    // Open evdev node
    ctx.fd = ::open(devnode.c_str(), O_RDWR | O_NONBLOCK);
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
    ctx.idev = idev;

    // Initialize frame buffer
    auto &state = AelkeyState::instance();
    state.frames[ctx.decl.id] = {};

    // Detect FF support
    if (libevdev_has_event_type(idev, EV_FF)) {
      ctx.haptics.supported = true;
      std::printf("Haptics: sink '%s' supports FF\n", ctx.decl.id.c_str());
    }

    std::cout << "Attached evdev: " << libevdev_get_name(idev) << std::endl;
    ctx.active = true;

    // Initial grab attempt
    if (ctx.decl.grab) {
      ctx.grab_pending = true;
      try_evdev_grab(ctx);
    }

    // Register FD with epoll
    register_fd(ctx.fd, EPOLLIN | EPOLLHUP | EPOLLERR);

    // store stable device ID
    devices_[ctx.fd] = ctx.decl.id;

    return true;
  }

  // Called by detach_input_device()
  void close_device(InputCtx &ctx) {
    // Unregister from epoll
    if (ctx.fd >= 0) {
      unregister_fd(ctx.fd);
      devices_.erase(ctx.fd);
    }

    // Free libevdev
    if (ctx.idev) {
      libevdev_grab(ctx.idev, LIBEVDEV_UNGRAB);
      libevdev_free(ctx.idev);
      ctx.idev = nullptr;
    }

    // Close FD
    if (ctx.fd >= 0) {
      close(ctx.fd);
      ctx.fd = -1;
    }

    ctx.active = false;
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
      InputDecl removed = detach_input_device(ctx.decl.id);
      if (!removed.id.empty()) {
        DispatcherUdev::instance().notify_state_change(removed, "remove");
      }
      return;
    }

    if (!(events & EPOLLIN)) {
      return;
    }

    dispatch_evdev_logic(ctx);
  }

 private:
  void dispatch_evdev_logic(InputCtx &ctx) {
    if (!ctx.idev) {
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
      int rc = libevdev_next_event(ctx.idev, LIBEVDEV_READ_FLAG_NORMAL, &ev);
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

  // fd → device id (stable)
  std::unordered_map<int, std::string> devices_;
};

template class Dispatcher<DispatcherEvdev>;

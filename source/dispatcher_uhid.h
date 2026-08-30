#pragma once

#include <map>
#include <string>

#include <fcntl.h>
#include <linux/uhid.h>
#include <sol/sol.hpp>
#include <sys/epoll.h>
#include <unistd.h>

#include "aelkey_state.h"
#include "dispatcher.h"

struct UhidDeviceContext {
  std::string id;
  std::string on_report;
};

class DispatcherUhid : public Dispatcher<DispatcherUhid> {
  friend class Singleton<DispatcherUhid>;
  friend class Dispatcher<DispatcherUhid>;

 protected:
  DispatcherUhid() = default;
  ~DispatcherUhid() = default;

 public:
  const char *type() const override {
    return "uhid";
  }

  void on_unregister(int fd) override {
    close(fd);
  }

  void register_source(int fd, const std::string &id, const std::string &on_report) {
    if (fd < 0) {
      return;
    }

    register_fd(fd, EPOLLIN);
    devices_[fd] = UhidDeviceContext{ id, on_report };
  }

  void remove_device(const std::string &id) {
    for (auto it = devices_.begin(); it != devices_.end(); ++it) {
      if (it->second.id == id) {
        int fd = it->first;
        unregister_fd(fd);
        devices_.erase(it);
        return;
      }
    }
  }

  void handle_event(EpollPayload *payload, uint32_t events) override {
    int fd = payload->fd;

    auto it = devices_.find(fd);
    if (it == devices_.end()) {
      return;
    }

    handle_uhid_event(fd, it->second, events);
  }

 private:
  void handle_uhid_event(int fd, const UhidDeviceContext &ctx, uint32_t events) {
    if (!(events & EPOLLIN)) {
      return;
    }

    struct uhid_event ev;
    ssize_t r = ::read(fd, &ev, sizeof(ev));
    if (r <= 0) {
      return;
    }

    switch (ev.type) {
      case UHID_OUTPUT:
      case UHID_GET_REPORT:
      case UHID_SET_REPORT:
        break;

      default:
        // Silently ignore UHID_START, UHID_STOP, UHID_OPEN, UHID_CLOSE
        return;
    }

    if (ctx.on_report.empty()) {
      switch (ev.type) {
        case UHID_GET_REPORT: {
          struct uhid_event reply{};
          reply.type = UHID_GET_REPORT_REPLY;
          reply.u.get_report_reply.id = ev.u.get_report.id;
          reply.u.get_report_reply.err = EIO;
          reply.u.get_report_reply.size = 0;

          if (write(fd, &reply, sizeof(reply)) < 0) {
            std::fprintf(
                stderr,
                "Failed to write UHID_GET_REPORT_REPLY for device ID %s: %s\n",
                ctx.id.c_str(),
                std::strerror(errno)
            );
          }
          break;
        }

        case UHID_SET_REPORT: {
          struct uhid_event reply{};
          reply.type = UHID_SET_REPORT_REPLY;
          reply.u.set_report_reply.id = ev.u.set_report.id;
          reply.u.set_report_reply.err = 0;

          if (write(fd, &reply, sizeof(reply)) < 0) {
            std::fprintf(
                stderr,
                "Failed to write UHID_SET_REPORT_REPLY for device ID %s: %s\n",
                ctx.id.c_str(),
                std::strerror(errno)
            );
          }
          break;
        }

        default:
          break;
      }

      return;
    }

    auto &state = AelkeyState::instance();
    sol::state_view lua(state.lua_vm);

    sol::object obj = lua[ctx.on_report];
    if (!obj.is<sol::function>()) {
      return;
    }

    sol::function cb = obj.as<sol::function>();

    sol::table tbl = lua.create_table();
    tbl["device"] = ctx.id;

    // Map the numerical event type to a descriptive string
    std::string type_str = "unknown";
    if (ev.type == UHID_OUTPUT) {
      type_str = "output";
      const char *data_ptr = reinterpret_cast<const char *>(ev.u.output.data);
      uint16_t data_size = ev.u.output.size;

      tbl["data"] = std::string_view(data_ptr, data_size);
      tbl["size"] = static_cast<int>(data_size);
      tbl["report_type"] = static_cast<int>(ev.u.output.rtype);

      if (data_size > 0) {
        tbl["report_id"] = static_cast<int>(static_cast<unsigned char>(data_ptr[0]));
      }
    } else if (ev.type == UHID_GET_REPORT) {
      type_str = "get_report";
      tbl["data"] = "";
      tbl["size"] = 0;
      tbl["trans_id"] = static_cast<int>(ev.u.get_report.id);
      tbl["report_id"] = static_cast<int>(ev.u.get_report.rnum);
      tbl["report_type"] = static_cast<int>(ev.u.get_report.rtype);
    } else if (ev.type == UHID_SET_REPORT) {
      type_str = "set_report";
      tbl["data"] = std::string_view(
          reinterpret_cast<const char *>(ev.u.set_report.data), ev.u.set_report.size
      );
      tbl["size"] = static_cast<int>(ev.u.set_report.size);
      tbl["trans_id"] = static_cast<int>(ev.u.set_report.id);
      tbl["report_id"] = static_cast<int>(ev.u.set_report.rnum);
      tbl["report_type"] = static_cast<int>(ev.u.set_report.rtype);
    }

    tbl["type"] = type_str;
    tbl["status"] = "ok";

    sol::protected_function pf = cb;
    sol::protected_function_result res = pf(tbl);
    if (!res.valid()) {
      sol::error err = res;
      std::fprintf(stderr, "Lua uhid on_report callback error: %s\n", err.what());
    }
  }

  std::map<int, UhidDeviceContext> devices_;
};

template class Dispatcher<DispatcherUhid>;

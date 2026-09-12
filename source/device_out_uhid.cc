#include "device_out_uhid.h"

#include <cstdio>
#include <cstring>

#include <fcntl.h>
#include <linux/uhid.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "aelkey_state.h"
#include "dispatcher_vulgate.h"

DeviceOutUhid::~DeviceOutUhid() {
  for (auto &[id, ctx] : devices_) {
    if (ctx.fd >= 0) {
      DispatcherVulgate::instance().unregister_device_fd(ctx.fd);
    }
  }
}

bool DeviceOutUhid::create(const OutputDecl &decl) {
  int fd = open("/dev/uhid", O_RDWR | O_CLOEXEC | O_NONBLOCK);
  if (fd < 0) {
    std::fprintf(stderr, "Failed to open /dev/uhid for device: %s\n", decl.name.c_str());
    return false;
  }

  struct uhid_event ev;
  std::memset(&ev, 0, sizeof(ev));
  ev.type = UHID_CREATE2;

  // Copy name
  std::strncpy(
      reinterpret_cast<char *>(ev.u.create2.name),
      decl.name.c_str(),
      sizeof(ev.u.create2.name) - 1
  );

  // Copy phys (Physical path, e.g., "virt/input0")
  if (!decl.phys.empty()) {
    std::strncpy(
        reinterpret_cast<char *>(ev.u.create2.phys),
        decl.phys.c_str(),
        sizeof(ev.u.create2.phys) - 1
    );
  }

  // Copy uniq (Unique identifier, e.g., serial number or MAC address)
  if (!decl.uniq.empty()) {
    std::strncpy(
        reinterpret_cast<char *>(ev.u.create2.uniq),
        decl.uniq.c_str(),
        sizeof(ev.u.create2.uniq) - 1
    );
  }

  ev.u.create2.rd_size = decl.report_desc.size();

  if (ev.u.create2.rd_size > sizeof(ev.u.create2.rd_data)) {
    std::fprintf(
        stderr, "UHID report descriptor too large for device: %s\n", decl.name.c_str()
    );
    close(fd);
    return false;
  }
  std::memcpy(ev.u.create2.rd_data, decl.report_desc.data(), ev.u.create2.rd_size);

  ev.u.create2.bus = decl.bus;
  ev.u.create2.vendor = decl.vendor;
  ev.u.create2.product = decl.product;
  ev.u.create2.version = decl.version;
  ev.u.create2.country = decl.country;

  if (write(fd, &ev, sizeof(ev)) < 0) {
    std::fprintf(stderr, "Failed to write UHID_CREATE2 for device: %s\n", decl.name.c_str());
    close(fd);
    return false;
  }

  devices_[decl.id] = DeviceContext{ .fd = fd, .on_report = decl.on_report };

  DispatcherCb cb;
  cb.native = [this, fd, id = decl.id, on_report = decl.on_report]() {
    handle_uhid_event(fd, id, on_report);
  };
  cb.cleanup = [](int fd_to_clean) {
    struct uhid_event destroy_ev;
    std::memset(&destroy_ev, 0, sizeof(destroy_ev));
    destroy_ev.type = UHID_DESTROY;
    if (write(fd_to_clean, &destroy_ev, sizeof(destroy_ev)) < 0) {
      // silence unused variable warning
    }
    close(fd_to_clean);
  };

  DispatcherVulgate::instance().register_device_fd(
      fd, EPOLLIN | EPOLLHUP | EPOLLERR, std::move(cb), decl.id
  );

  std::printf("Created uhid device: %s (id: %s)\n", decl.name.c_str(), decl.id.c_str());
  return true;
}

int DeviceOutUhid::get_fd(const std::string &id) const {
  auto it = devices_.find(id);
  return (it != devices_.end()) ? it->second.fd : -1;
}

void DeviceOutUhid::write_report(
    const std::string &id,
    const std::string &type,
    const std::string &data
) {
  int fd = get_fd(id);
  if (fd < 0) {
    return;
  }

  struct uhid_event ev;
  std::memset(&ev, 0, sizeof(ev));
  ev.type = UHID_INPUT2;

  ev.u.input2.size = data.size();
  if (ev.u.input2.size > sizeof(ev.u.input2.data)) {
    std::fprintf(stderr, "UHID input report payload too large for device ID: %s\n", id.c_str());
    return;
  }
  std::memcpy(ev.u.input2.data, data.data(), ev.u.input2.size);

  if (write(fd, &ev, sizeof(ev)) < 0) {
    std::fprintf(
        stderr,
        "Failed to write UHID report for device ID %s: %s\n",
        id.c_str(),
        std::strerror(errno)
    );
  }
}

void DeviceOutUhid::reply(
    const std::string &id,
    uint32_t trans_id,
    int status,
    const std::string &data
) {
  int fd = get_fd(id);
  if (fd < 0) {
    return;
  }

  struct uhid_event ev;
  std::memset(&ev, 0, sizeof(ev));

  ev.type = UHID_GET_REPORT_REPLY;
  ev.u.get_report_reply.id = trans_id;
  ev.u.get_report_reply.err = status;

  ev.u.get_report_reply.size = data.size();
  if (ev.u.get_report_reply.size > sizeof(ev.u.get_report_reply.data)) {
    ev.u.get_report_reply.size = sizeof(ev.u.get_report_reply.data);
  }
  std::memcpy(ev.u.get_report_reply.data, data.data(), ev.u.get_report_reply.size);

  if (write(fd, &ev, sizeof(ev)) < 0) {
    std::fprintf(
        stderr,
        "Failed to write UHID reply for device ID %s: %s\n",
        id.c_str(),
        std::strerror(errno)
    );
  }
}

void DeviceOutUhid::handle_uhid_event(
    int fd,
    const std::string &id,
    const std::string &on_report
) {
  struct uhid_event ev;
  if (read(fd, &ev, sizeof(ev)) <= 0) {
    return;
  }

  switch (ev.type) {
    case UHID_OUTPUT:
    case UHID_GET_REPORT:
    case UHID_SET_REPORT:
      break;

    default:
      return;
  }

  if (on_report.empty()) {
    switch (ev.type) {
      case UHID_GET_REPORT: {
        struct uhid_event reply_ev{};
        reply_ev.type = UHID_GET_REPORT_REPLY;
        reply_ev.u.get_report_reply.id = ev.u.get_report.id;
        reply_ev.u.get_report_reply.err = EIO;
        reply_ev.u.get_report_reply.size = 0;

        if (write(fd, &reply_ev, sizeof(reply_ev)) < 0) {
          std::fprintf(
              stderr,
              "Failed to write UHID_GET_REPORT_REPLY for device ID %s: %s\n",
              id.c_str(),
              std::strerror(errno)
          );
        }
        break;
      }

      case UHID_SET_REPORT: {
        struct uhid_event reply_ev{};
        reply_ev.type = UHID_SET_REPORT_REPLY;
        reply_ev.u.set_report_reply.id = ev.u.set_report.id;
        reply_ev.u.set_report_reply.err = 0;

        if (write(fd, &reply_ev, sizeof(reply_ev)) < 0) {
          std::fprintf(
              stderr,
              "Failed to write UHID_SET_REPORT_REPLY for device ID %s: %s\n",
              id.c_str(),
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

  sol::object obj = lua[on_report];
  if (!obj.is<sol::function>()) {
    return;
  }

  sol::function cb = obj.as<sol::function>();

  sol::table tbl = lua.create_table();
  tbl["device"] = id;

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

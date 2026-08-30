#include "device_out_uhid.h"

#include <cstdio>
#include <cstring>
#include <iostream>

#include <fcntl.h>
#include <linux/uhid.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "dispatcher_uhid.h"

DeviceOutUhid::~DeviceOutUhid() {
  for (auto &[id, ctx] : devices_) {
    DispatcherUhid::instance().remove_device(id);

    if (ctx.fd >= 0) {
      struct uhid_event ev;
      std::memset(&ev, 0, sizeof(ev));
      ev.type = UHID_DESTROY;
      write(ctx.fd, &ev, sizeof(ev));
      close(ctx.fd);
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

  ssize_t ret = write(fd, &ev, sizeof(ev));
  if (ret < 0) {
    std::fprintf(stderr, "Failed to write UHID_CREATE2 for device: %s\n", decl.name.c_str());
    close(fd);
    return false;
  }

  devices_[decl.id] = DeviceContext{ .fd = fd, .on_report = decl.on_report };

  DispatcherUhid::instance().register_source(fd, decl.id, decl.on_report);

  std::cout << "Created uhid device: " << decl.name << " (id: " << decl.id << ")" << std::endl;
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

  write(fd, &ev, sizeof(ev));
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

  write(fd, &ev, sizeof(ev));
}

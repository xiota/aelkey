#include "device_in_gatt.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include <sol/sol.hpp>
#include <unistd.h>

#include "aelkey_state.h"
#include "backend_bluez.h"
#include "dispatcher_vulgate.h"
#include "manager_device_in.h"

bool DeviceInGatt::on_init() {
  auto &bluez = BackendBluez::instance();
  if (!bluez.lazy_init()) {
    return false;
  }

  return true;
}

bool DeviceInGatt::match(InputDecl &decl, std::string &devnode_out) {
  if (!lazy_init()) {
    return false;
  }

  if (decl.type != "gatt") {
    return false;
  }

  auto &bluez = BackendBluez::instance();
  std::string out = bluez.resolve_gatt_paths(decl, nullptr);
  if (out.empty()) {
    return false;
  }
  devnode_out = out;
  return true;
}

bool DeviceInGatt::attach(const std::string &devnode, InputDecl &decl) {
  if (!lazy_init()) {
    return false;
  }

  auto &bluez = BackendBluez::instance();
  std::vector<std::string> found_characteristics;

  GattPathType type = BackendBluez::classify_gatt_path(devnode);
  if (type == GattPathType::Characteristic) {
    found_characteristics.push_back(devnode);
  } else {
    bluez.resolve_gatt_paths(decl, &found_characteristics);
  }

  bool matched = false;
  for (const auto &ch : found_characteristics) {
    bluez.print_characteristic_inspect_line(ch);

    if (bluez.characteristic_supports_notify(ch)) {
      auto session = bluez.acquire_notify(ch);
      if (session.fd >= 0) {
        fd_to_id_[session.fd] = decl.id;

        DispatcherCb cb;
        cb.native = [this, fd = session.fd, id = decl.id, path = ch, mtu = session.mtu]() {
          handle_gatt_event(fd, id, path, mtu);
        };
        cb.cleanup = [this](int fd_to_clean) {
          fd_to_id_.erase(fd_to_clean);
          close(fd_to_clean);
        };

        DispatcherVulgate::instance().register_device_fd(
            session.fd, EPOLLIN | EPOLLHUP | EPOLLERR, std::move(cb), decl.id
        );

        decl.subbed_chars.insert(ch);
        matched = true;
      }
    }
  }

  if (matched) {
    decl.devnode = devnode;
    gatt_paths_[decl.id] = (type == GattPathType::Characteristic)
                               ? BackendBluez::derive_device_path_from_char_path(devnode)
                               : devnode;
  }

  return matched;
}

bool DeviceInGatt::detach(const std::string &id) {
  auto &state = AelkeyState::instance();
  auto it = state.input_map.find(id);
  if (it == state.input_map.end()) {
    return false;
  }

  InputDecl &decl = it->second;

  // Unregister all file descriptors tied to this device from DispatcherVulgate
  for (auto fd_it = fd_to_id_.begin(); fd_it != fd_to_id_.end();) {
    if (fd_it->second == id) {
      DispatcherVulgate::instance().unregister_device_fd(fd_it->first);
      fd_it = fd_to_id_.erase(fd_it);
    } else {
      ++fd_it;
    }
  }

  if (!decl.devnode.empty()) {
    BackendBluez::instance().disconnect_device(decl.devnode);
    gatt_paths_.erase(id);
  }
  return true;
}

void DeviceInGatt::handle_gatt_event(
    int fd,
    const std::string &id,
    const std::string &path,
    uint16_t mtu
) {
  auto &state = AelkeyState::instance();
  auto decl_it = state.input_map.find(id);
  if (decl_it == state.input_map.end()) {
    return;
  }
  InputDecl &decl = decl_it->second;

  // Read raw ATT packet directly from socket
  std::vector<uint8_t> buffer(mtu > 0 ? mtu : 512);
  ssize_t n = read(fd, buffer.data(), buffer.size());
  if (n <= 0) {
    return;
  }
  buffer.resize(n);

  if (decl.on_event.empty()) {
    return;
  }

  sol::state_view lua(state.lua_vm);
  sol::object obj = lua[decl.on_event];
  if (!obj.is<sol::function>()) {
    return;
  }

  sol::function cb = obj.as<sol::function>();
  sol::table tbl = lua.create_table();
  tbl["device"] = decl.id;
  tbl["path"] = path;
  tbl["data"] = std::string_view(reinterpret_cast<const char *>(buffer.data()), buffer.size());
  tbl["size"] = static_cast<int>(buffer.size());
  tbl["status"] = "ok";

  sol::protected_function pf = cb;
  sol::protected_function_result res = pf(tbl);
  if (!res.valid()) {
    sol::error err = res;
    std::fprintf(stderr, "Lua gatt_callback error: %s\n", err.what());
  }
}

#include "device_in_gatt.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include <sol/sol.hpp>

#include "aelkey_state.h"
#include "backend_bluez.h"
#include "dispatcher_gatt.h"

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
        if (DispatcherGatt::instance().open_device_notify(ch, decl, session.fd, session.mtu)) {
          decl.subbed_chars.insert(ch);
          matched = true;
        } else {
          close(session.fd);
        }
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
  DispatcherGatt::instance().close_device(decl);

  if (!decl.devnode.empty()) {
    BackendBluez::instance().disconnect_device(decl.devnode);
    gatt_paths_.erase(id);
  }
  return true;
}

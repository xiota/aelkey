#include "device_in_gatt.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include <sol/sol.hpp>

#include "aelkey_state.h"
#include "backend_bluez.h"
#include "tick_scheduler.h"

DeviceInGatt::~DeviceInGatt() {
  if (tick_fd_ >= 0) {
    TickScheduler::instance().unregister_fd(tick_fd_);
    tick_fd_ = -1;
  }
}

bool DeviceInGatt::on_init() {
  auto &bluez = BackendBluez::instance();
  if (!bluez.lazy_init()) {
    return false;
  }

  tok_gatt_ = bluez.sig_gatt_value_.subscribe(
      [this](const std::string &path, const std::vector<uint8_t> &data) {
        queue_.enqueue(GattEvent{ path, data });
        if (tick_fd_ >= 0) {
          TickScheduler::instance().trigger(tick_fd_);
        }
      }
  );

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

  bool matched = false;

  if (devnode.empty()) {
    std::fprintf(stderr, "GATT: no GATT path in devnode for %s\n", decl.id.c_str());
    return false;
  }

  auto &bluez = BackendBluez::instance();
  GattPathType type = BackendBluez::classify_gatt_path(devnode);

  std::string gatt_path;
  if (type == GattPathType::Characteristic) {
    gatt_path = BackendBluez::derive_device_path_from_char_path(devnode);
    if (gatt_path.empty()) {
      std::fprintf(stderr, "GATT: failed to derive device path from %s\n", devnode.c_str());
    }
  } else {
    gatt_path = devnode;
  }

  if (type != GattPathType::Characteristic) {
    std::vector<std::string> found_characteristics;
    bluez.resolve_gatt_paths(decl, &found_characteristics);

    for (const auto &ch : found_characteristics) {
      bluez.print_characteristic_inspect_line(ch);

      if (bluez.characteristic_supports_notify(ch)) {
        if (bluez.start_notify(ch)) {
          decl.subbed_chars.insert(ch);
          matched = true;
        }
      }
    }
  } else {
    bluez.print_characteristic_inspect_line(devnode);
    if (bluez.start_notify(devnode)) {
      decl.subbed_chars.insert(devnode);
      matched = true;
    }
  }

  gatt_paths_[decl.id] = gatt_path;

  if (matched) {
    decl.devnode = devnode;

    if (tick_fd_ < 0) {
      TickCb cb;
      cb.native = [this]() { this->pump_messages(); };
      cb.oneshot = false;

      tick_fd_ = TickScheduler::instance().schedule(10000, cb);
      if (tick_fd_ < 0) {
        std::fprintf(stderr, "GATT: failed to schedule tick\n");
      }
    }
  }

  return matched;
}

bool DeviceInGatt::detach(const std::string &id) {
  if (!lazy_init()) {
    return false;
  }

  auto &state = AelkeyState::instance();
  auto it = state.input_map.find(id);
  if (it == state.input_map.end()) {
    return false;
  }

  InputDecl &decl = it->second;

  auto &bluez = BackendBluez::instance();

  std::vector<std::string> to_remove;

  for (const auto &char_path : decl.subbed_chars) {
    bluez.stop_notify(char_path);
    to_remove.push_back(char_path);
  }

  for (const auto &char_path : to_remove) {
    decl.subbed_chars.erase(char_path);
  }

  if (!decl.devnode.empty()) {
    bluez.disconnect_device(decl.devnode);
    gatt_paths_.erase(id);
    decl.devnode.clear();
  }
  return true;
}

void DeviceInGatt::pump_messages() {
  if (!lazy_init()) {
    return;
  }

  auto &state = AelkeyState::instance();
  sol::state_view lua(state.lua_vm);

  GattEvent ev;
  while (queue_.try_dequeue(ev)) {
    const char *path = ev.path.c_str();

    for (auto &[_, decl] : state.input_map) {
      if (decl.type != "gatt") {
        continue;
      }

      if (decl.on_event.empty()) {
        continue;
      }

      sol::object obj = lua[decl.on_event];
      if (!obj.is<sol::function>()) {
        continue;
      }

      sol::function cb = obj.as<sol::function>();

      sol::table tbl = lua.create_table();
      tbl["device"] = decl.id;
      tbl["path"] = path;
      tbl["data"] =
          std::string_view(reinterpret_cast<const char *>(ev.data.data()), ev.data.size());
      tbl["size"] = static_cast<int>(ev.data.size());
      tbl["status"] = "ok";

      sol::protected_function pf = cb;
      sol::protected_function_result res = pf(tbl);
      if (!res.valid()) {
        sol::error err = res;
        std::fprintf(stderr, "Lua gatt_callback error: %s\n", err.what());
      }

      break;
    }
  }
}

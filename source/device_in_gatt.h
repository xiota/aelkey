#pragma once

#include <cstddef>
#include <cstdint>
#include <format>
#include <map>
#include <string>
#include <vector>

#include <sol/sol.hpp>

#include "backend_bluez.h"
#include "device_declarations.h"
#include "device_in.h"
#include "singleton.h"
#include "utils/lua_helpers.h"

struct GattEvent {
  std::string path;
  std::vector<uint8_t> data;
};

struct GattEventPayload {
  std::string device;
  std::string path;
  std::string_view data;
  int size;
  std::string status;
  uint64_t timestamp;

  sol::table to_lua(sol::state_view lua) const {
    sol::table t = lua.create_table();
    AelkeyUtil::lua_set_field(t, "device", device);
    AelkeyUtil::lua_set_field(t, "path", path);
    AelkeyUtil::lua_set_field(t, "data", data);
    AelkeyUtil::lua_set_field(t, "size", size);
    AelkeyUtil::lua_set_field(t, "status", status);
    AelkeyUtil::lua_set_field(t, "timestamp", timestamp);
    return t;
  }
};

class DeviceInGatt : public DeviceIn, public Singleton<DeviceInGatt> {
  friend class Singleton<DeviceInGatt>;

 protected:
  DeviceInGatt() = default;
  ~DeviceInGatt() = default;

  bool on_init() override;

 public:
  bool match(InputDecl &decl, std::string &devnode_out) override;

  bool attach(const std::string &devnode, InputDecl &decl) override;

  bool detach(const std::string &id) override;

  // Public API used by Lua wrappers
  bool read_characteristic(const std::string &char_path, std::vector<uint8_t> &out_data) {
    return BackendBluez::instance().read_characteristic(char_path, out_data);
  }

  bool write_characteristic(
      const std::string &char_path,
      const uint8_t *data,
      size_t len,
      bool with_resp
  ) {
    return BackendBluez::instance().write_characteristic(char_path, data, len, with_resp);
  }

  // Resolve characteristic path using optional service/characteristic overrides
  std::string
  resolve_char_path(const std::string &id, int service = -1, int characteristic = -1) {
    std::string gp = get_gatt_path(id);
    if (gp.empty()) {
      return {};
    }

    // No overrides → use primary characteristic
    if (service <= 0 && characteristic <= 0) {
      return gp;
    }

    // Overrides must both be provided
    if (service <= 0 || characteristic <= 0) {
      return {};
    }

    // Construct BlueZ object path:
    // /org/bluez/hci0/dev_xx/serviceXXXX/charYYYY
    return std::format("{}/service{:04X}/char{:04X}", gp, service, characteristic);
  }

  std::string get_gatt_path(const std::string &id) const {
    auto it = gatt_paths_.find(id);
    if (it == gatt_paths_.end()) {
      return {};
    }
    return it->second;
  }

 private:
  void handle_gatt_event(int fd, const std::string &id, const std::string &path, uint16_t mtu);

  // dev_id -> gatt_path, /org/bluez/hci0/dev_XX_XX_XX_XX_XX_XX
  std::map<std::string, std::string> gatt_paths_;
  std::map<int, std::string> fd_to_id_;
};

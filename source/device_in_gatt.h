#pragma once

#include <cstddef>
#include <cstdint>
#include <format>
#include <map>
#include <string>
#include <vector>

#include <readerwriterqueue.h>

#include "backend_bluez.h"
#include "device_declarations.h"
#include "device_in.h"
#include "singleton.h"

struct GattEvent {
  std::string path;
  std::vector<uint8_t> data;
};

class DeviceInGatt : public DeviceIn, public Singleton<DeviceInGatt> {
  friend class Singleton<DeviceInGatt>;

 protected:
  DeviceInGatt() = default;
  ~DeviceInGatt();

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

  // --- message dispatch ---
  void pump_messages();

 private:
  // dev_id -> gatt_path, /org/bluez/hci0/dev_XX_XX_XX_XX_XX_XX
  std::map<std::string, std::string> gatt_paths_;

  int tick_fd_ = -1;

  moodycamel::ReaderWriterQueue<GattEvent> queue_;

  AelkeyUtil::Signal<void(const std::string &, const std::vector<uint8_t> &)>::Connection
      tok_gatt_;
};

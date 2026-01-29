#pragma once

#include <format>
#include <map>
#include <string>
#include <vector>

#include <dbus/dbus.h>

#include "aelkey_state.h"
#include "device_backend.h"
#include "device_declarations.h"
#include "singleton.h"

enum class GattPathType { Device, Service, Characteristic };

class DeviceBackendGATT : public DeviceBackend, public Singleton<DeviceBackendGATT> {
  friend class Singleton<DeviceBackendGATT>;

 protected:
  DeviceBackendGATT() = default;
  ~DeviceBackendGATT() {
    if (conn_) {
      dbus_connection_unref(conn_);
      conn_ = nullptr;
    }
  }

 public:
  // device_backend_gatt.h (inside public:)
  bool match(const InputDecl &decl, std::string &devnode_out) override {
    if (decl.type != "gatt") {
      return false;
    }

    std::string out = resolve_gatt_paths(decl, nullptr);
    if (out.empty()) {
      return false;
    }
    devnode_out = out;
    return true;
  }

  bool attach(const std::string &devnode, InputDecl &decl) override {
    ensure_initialized();
    if (!conn_) {
      std::fprintf(stderr, "GATT: no D-Bus connection\n");
      return false;
    }

    if (devnode.empty()) {
      std::fprintf(stderr, "GATT: no GATT path in devnode for %s\n", decl.id.c_str());
      return false;
    }

    GattPathType type = classify_gatt_path(devnode);

    std::string gatt_path;
    if (type == GattPathType::Characteristic) {
      gatt_path = derive_device_path_from_char_path(devnode);
      if (gatt_path.empty()) {
        std::fprintf(stderr, "GATT: failed to derive device path from %s\n", devnode.c_str());
      }
    } else {
      gatt_path = devnode;
    }

    if (type != GattPathType::Characteristic) {
      std::vector<std::string> found_characteristics;
      resolve_gatt_paths(decl, &found_characteristics);

      for (const auto &ch : found_characteristics) {
        print_characteristic_inspect_line(ch);

        if (characteristic_supports_notify(ch)) {
          std::string rule =
              "type='signal',interface='org.freedesktop.DBus.Properties',"
              "member='PropertiesChanged',path='" +
              ch + "'";

          dbus_bus_add_match(conn_, rule.c_str(), nullptr);
          start_notify(ch);
        }
      }

      dbus_connection_flush(conn_);
    } else {
      print_characteristic_inspect_line(devnode);

      std::string rule =
          "type='signal',interface='org.freedesktop.DBus.Properties',"
          "member='PropertiesChanged',path='" +
          devnode + "'";

      dbus_bus_add_match(conn_, rule.c_str(), nullptr);
      dbus_connection_flush(conn_);

      start_notify(devnode);
    }

    gatt_paths_[decl.id] = gatt_path;

    decl.devnode = devnode;
    return true;
  }

  bool detach(const std::string &id) override {
    if (!conn_) {
      return false;
    }

    auto &state = AelkeyState::instance();
    auto it = state.input_map.find(id);
    if (it == state.input_map.end()) {
      return false;
    }

    InputDecl &decl = it->second;
    if (!decl.devnode.empty()) {
      stop_notify(decl.devnode);
    }

    gatt_paths_.erase(id);

    return true;
  }

  // Public API used by Lua wrappers
  bool read_characteristic(const std::string &char_path, std::vector<uint8_t> &out_data);
  bool write_characteristic(
      const std::string &char_path,
      const uint8_t *data,
      size_t len,
      bool with_resp
  );

  std::string get_gatt_path(const std::string &id) const {
    auto it = gatt_paths_.find(id);
    if (it == gatt_paths_.end()) {
      return {};
    }
    return it->second;
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

  // Optional: expose for advanced callers
  DBusConnection *connection() const {
    return conn_;
  }

  // --- init helpers ---
  void ensure_initialized();

  // --- message dispatch ---
  void pump_messages();

 private:
  // --- state ---
  DBusConnection *conn_ = nullptr;

  // --- message dispatch ---
  void process_one_message(DBusMessage *msg);

  // --- notify helpers ---
  void start_notify(const std::string &char_path);
  void stop_notify(const std::string &char_path);

  // --- path helpers ---
  static GattPathType classify_gatt_path(const std::string &path);
  static std::string derive_device_path_from_char_path(const std::string &char_path);

  // --- D-Bus helpers ---
  DBusMessage *get_managed_objects();

  std::string get_characteristic_uuid(const std::string &path);
  std::vector<std::string> get_characteristic_flags(const std::string &path);
  void print_characteristic_inspect_line(const std::string &ch);

  bool characteristic_supports_notify(const std::string &char_path);

  // --- matching helpers ---
  std::string
  resolve_gatt_paths(const InputDecl &decl, std::vector<std::string> *found_characteristics);

  std::vector<std::string> get_matching_devices(const InputDecl &decl, DBusMessageIter &array);
  std::vector<std::string> get_matching_services(
      const InputDecl &decl,
      const std::vector<std::string> &candidate_devices,
      DBusMessageIter &array
  );
  std::vector<std::string> get_matching_characteristics(
      const InputDecl &decl,
      const std::vector<std::string> &candidate_services,
      DBusMessageIter &array
  );

 private:
  // dev_id -> gatt_path, /org/bluez/hci0/dev_XX_XX_XX_XX_XX_XX
  std::map<std::string, std::string> gatt_paths_;
};

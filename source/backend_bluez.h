#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <dbus/dbus.h>

#include "device_declarations.h"
#include "singleton.h"

enum class GattPathType { Device, Service, Characteristic };

class BackendBluez : public Singleton<BackendBluez> {
  friend class Singleton<BackendBluez>;

 private:
  BackendBluez() = default;
  ~BackendBluez();

 public:
  bool ensure_client();

  DBusConnection *connection() const {
    return conn_;
  }

  int fd() const {
    return fd_;
  }

  // --- D-Bus / BlueZ operations ---
  void start_notify(const std::string &char_path);
  void stop_notify(const std::string &char_path);

  bool read_characteristic(const std::string &char_path, std::vector<uint8_t> &out_data);
  bool write_characteristic(
      const std::string &char_path,
      const uint8_t *data,
      size_t len,
      bool with_resp
  );

  bool characteristic_supports_notify(const std::string &char_path);

  void add_match_rule(const std::string &rule);

  // --- Path resolution & inspection ---
  static GattPathType classify_gatt_path(const std::string &path);
  static std::string derive_device_path_from_char_path(const std::string &char_path);

  std::string get_characteristic_uuid(const std::string &path);
  std::vector<std::string> get_characteristic_flags(const std::string &path);
  void print_characteristic_inspect_line(const std::string &ch);

  std::string
  resolve_gatt_paths(const InputDecl &decl, std::vector<std::string> *found_characteristics);

  DBusMessage *get_managed_objects();

 private:
  bool on_init() override;

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
  DBusConnection *conn_ = nullptr;
  int fd_ = -1;
};

#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include <sdbus-c++/sdbus-c++.h>

#include "device_declarations.h"
#include "singleton.h"
#include "utils/signal.h"

enum class GattPathType { Device, Service, Characteristic };

class BackendBluez : public Singleton<BackendBluez> {
  friend class Singleton<BackendBluez>;

 private:
  BackendBluez() = default;
  ~BackendBluez() = default;

  bool ensure_client();

 public:
  void shutdown();

  // --- D-Bus / BlueZ operations ---
  bool start_notify(const std::string &char_path);
  void stop_notify(const std::string &char_path);

  bool read_characteristic(const std::string &char_path, std::vector<uint8_t> &out_data);
  bool write_characteristic(
      const std::string &char_path,
      const uint8_t *data,
      size_t len,
      bool with_resp
  );

  bool characteristic_supports_notify(const std::string &char_path);

  bool disconnect_device(const std::string &path);

  // --- Path resolution & inspection ---
  static GattPathType classify_gatt_path(const std::string &path);
  static std::string derive_device_path_from_char_path(const std::string &char_path);

  std::string get_characteristic_uuid(const std::string &path);
  std::vector<std::string> get_characteristic_flags(const std::string &path);
  void print_characteristic_inspect_line(const std::string &ch);

  std::string
  resolve_gatt_paths(const InputDecl &decl, std::vector<std::string> *found_characteristics);

  // Signal: path, value bytes
  AelkeyUtil::Signal<void(const std::string &, const std::vector<uint8_t> &)> sig_gatt_value_;

 private:
  bool on_init() override;

  using ManagedObjects =
      std::map<sdbus::ObjectPath, std::map<std::string, std::map<std::string, sdbus::Variant>>>;

  ManagedObjects get_managed_objects();

  std::vector<std::string>
  get_matching_devices(const InputDecl &decl, const ManagedObjects &objs);
  std::vector<std::string> get_matching_services(
      const InputDecl &decl,
      const std::vector<std::string> &candidate_devices,
      const ManagedObjects &objs
  );
  std::vector<std::string> get_matching_characteristics(
      const InputDecl &decl,
      const std::vector<std::string> &candidate_services,
      const ManagedObjects &objs
  );

  void on_device_properties_changed(sdbus::Message &msg);
  void on_properties_changed(sdbus::Message &msg);

 private:
  std::unique_ptr<sdbus::IConnection> conn_;
  std::unordered_map<std::string, sdbus::Slot> match_slots_;
};

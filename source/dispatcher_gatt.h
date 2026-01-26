#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include <dbus/dbus.h>
#include <sol/sol.hpp>

#include "aelkey_state.h"
#include "device_input.h"
#include "dispatcher.h"
#include "dispatcher_registry.h"

enum class GattPathType { Device, Service, Characteristic };

class DispatcherGATT : public Dispatcher<DispatcherGATT> {
  friend class Singleton<DispatcherGATT>;

 protected:
  DispatcherGATT() = default;
  ~DispatcherGATT() {
    // We do NOT own fd_ (D-Bus owns it), so just unregister if needed
    if (fd_ >= 0) {
      unregister_fd(fd_);
      fd_ = -1;
    }
    if (conn_) {
      dbus_connection_unref(conn_);
      conn_ = nullptr;
    }
  }

 public:
  const char *type() const override {
    return "gatt";
  }

  void init() override {
    ensure_initialized();
  }

  void handle_event(EpollPayload *payload, uint32_t events) override {
    if (!(events & EPOLLIN) || !conn_) {
      return;
    }

    // We need a Lua state; same pattern as others: use AelkeyState::lua_vm
    sol::state_view lua(AelkeyState::instance().lua_vm);
    sol::this_state ts(lua.lua_state());
    pump_messages(ts);
  }

  // Public API used by device_input / Lua wrappers
  InputCtx attach_device(const InputDecl &decl);
  void detach_device(InputCtx &ctx);

  std::string match_device(
      const InputDecl &decl,
      std::vector<std::string> *found_characteristics = nullptr
  );

  bool read_characteristic(const std::string &char_path, std::vector<uint8_t> &out_data);
  bool write_characteristic(
      const std::string &char_path,
      const uint8_t *data,
      size_t len,
      bool with_resp
  );

  // Optional: expose for advanced callers
  DBusConnection *connection() const {
    return conn_;
  }

 private:
  // --- state ---
  DBusConnection *conn_ = nullptr;
  int fd_ = -1;

  // --- init helpers ---
  void ensure_initialized();

  // --- message dispatch ---
  void pump_messages(sol::this_state ts);
  void process_one_message(sol::this_state ts, DBusMessage *msg);

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
};

template class Dispatcher<DispatcherGATT>;

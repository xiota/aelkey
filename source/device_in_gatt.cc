#include "device_in_gatt.h"

#include <cstring>
#include <iostream>
#include <string>
#include <string_view>

#include <dbus/dbus.h>
#include <sol/sol.hpp>

#include "aelkey_state.h"
#include "backend_bluez.h"
#include "dispatcher_gatt.h"
#include "manager_device_in.h"

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
        std::string rule =
            "type='signal',"
            "interface='org.freedesktop.DBus.Properties',"
            "member='PropertiesChanged',"
            "path='" +
            ch + "'";

        bluez.add_match_rule(rule);
        bluez.start_notify(ch);
      }
    }
  } else {
    bluez.print_characteristic_inspect_line(devnode);

    std::string rule =
        "type='signal',"
        "interface='org.freedesktop.DBus.Properties',"
        "member='PropertiesChanged',"
        "path='" +
        devnode + "'";

    bluez.add_match_rule(rule);
    bluez.start_notify(devnode);
  }

  gatt_paths_[decl.id] = gatt_path;

  decl.devnode = devnode;
  return true;
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
  if (!decl.devnode.empty()) {
    BackendBluez::instance().stop_notify(decl.devnode);
  }

  gatt_paths_.erase(id);

  return true;
}

bool DeviceInGatt::on_init() {
  auto &bluez = BackendBluez::instance();
  if (!bluez.lazy_init()) {
    return false;
  }

  fd_ = bluez.fd();

  std::string rule =
      "type='signal',"
      "sender='org.bluez',"
      "interface='org.freedesktop.DBus.Properties',"
      "member='PropertiesChanged'";
  bluez.add_match_rule(rule);

  return DispatcherGATT::instance().lazy_init();
}

static void process_one_message(DBusMessage *msg) {
  auto &state = AelkeyState::instance();
  sol::state_view lua(state.lua_vm);

  const char *path = dbus_message_get_path(msg);
  if (!path) {
    return;
  }

  DBusMessageIter args;
  dbus_message_iter_init(msg, &args);

  const char *iface = nullptr;
  if (dbus_message_iter_get_arg_type(&args) == DBUS_TYPE_STRING) {
    dbus_message_iter_get_basic(&args, &iface);
  }

  if (iface && strcmp(iface, "org.bluez.Device1") == 0) {
    dbus_message_iter_next(&args);
    if (dbus_message_iter_get_arg_type(&args) == DBUS_TYPE_ARRAY) {
      DBusMessageIter dict;
      dbus_message_iter_recurse(&args, &dict);

      while (dbus_message_iter_get_arg_type(&dict) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter entry;
        dbus_message_iter_recurse(&dict, &entry);

        const char *key = nullptr;
        if (dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_STRING) {
          dbus_message_iter_get_basic(&entry, &key);
        }

        dbus_message_iter_next(&entry);
        if (key && strcmp(key, "ServicesResolved") == 0 &&
            dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_VARIANT) {
          DBusMessageIter variant;
          dbus_message_iter_recurse(&entry, &variant);

          if (dbus_message_iter_get_arg_type(&variant) == DBUS_TYPE_BOOLEAN) {
            dbus_bool_t services_resolved = FALSE;
            dbus_message_iter_get_basic(&variant, &services_resolved);

            if (services_resolved) {
              auto &devmgr = ManagerDeviceIn::instance();
              for (auto &decl : state.input_decls) {
                if (decl.type == "gatt") {
                  // Skip if already attached
                  if (!decl.devnode.empty()) {
                    continue;
                  }

                  std::string devnode;
                  if (devmgr.match(decl, devnode)) {
                    if (devmgr.attach(devnode, decl)) {
                      decl.devnode = devnode;
                    }
                  }
                }
              }
            }
          }
        }
        dbus_message_iter_next(&dict);
      }
    }
    return;
  }

  if (!iface || strcmp(iface, "org.bluez.GattCharacteristic1") != 0) {
    return;
  }

  std::vector<uint8_t> bytes;

  dbus_message_iter_next(&args);
  DBusMessageIter dict;
  dbus_message_iter_recurse(&args, &dict);

  while (dbus_message_iter_get_arg_type(&dict) == DBUS_TYPE_DICT_ENTRY) {
    DBusMessageIter entry;
    dbus_message_iter_recurse(&dict, &entry);

    const char *key = nullptr;
    if (dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_STRING) {
      dbus_message_iter_get_basic(&entry, &key);
    }

    dbus_message_iter_next(&entry);
    if (dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_VARIANT) {
      DBusMessageIter variant;
      dbus_message_iter_recurse(&entry, &variant);

      if (key && strcmp(key, "Value") == 0) {
        if (dbus_message_iter_get_arg_type(&variant) == DBUS_TYPE_ARRAY) {
          DBusMessageIter array;
          dbus_message_iter_recurse(&variant, &array);

          while (dbus_message_iter_get_arg_type(&array) == DBUS_TYPE_BYTE) {
            uint8_t b;
            dbus_message_iter_get_basic(&array, &b);
            bytes.push_back(b);
            dbus_message_iter_next(&array);
          }
        }
      }
    }

    dbus_message_iter_next(&dict);
  }

  for (auto &[_, decl] : state.input_map) {
    if (decl.type != "gatt") {
      continue;
    }

    if (!decl.on_event.empty()) {
      sol::object obj = lua[decl.on_event];
      if (!obj.is<sol::function>()) {
        continue;
      }

      sol::function cb = obj.as<sol::function>();

      sol::table tbl = lua.create_table();
      tbl["device"] = decl.id;
      tbl["path"] = path;
      tbl["data"] =
          std::string_view(reinterpret_cast<const char *>(bytes.data()), bytes.size());
      tbl["size"] = static_cast<int>(bytes.size());
      tbl["status"] = "ok";

      sol::protected_function pf = cb;
      sol::protected_function_result res = pf(tbl);
      if (!res.valid()) {
        sol::error err = res;
        std::fprintf(stderr, "Lua gatt_callback error: %s\n", err.what());
      }
    }

    break;
  }
}

void DeviceInGatt::pump_messages() {
  if (!lazy_init()) {
    return;
  }

  DBusConnection *conn = BackendBluez::instance().connection();
  if (!conn) {
    return;
  }

  // Non-blocking read
  dbus_connection_read_write(conn, 0);

  // Process ALL pending messages
  while (true) {
    DBusMessage *msg = dbus_connection_pop_message(conn);
    if (!msg) {
      break;
    }

    process_one_message(msg);
    dbus_message_unref(msg);
  }
}

bool DeviceInGatt::read_characteristic(
    const std::string &char_path,
    std::vector<uint8_t> &out_data
) {
  if (!lazy_init()) {
    return false;
  }
  return BackendBluez::instance().read_characteristic(char_path, out_data);
}

bool DeviceInGatt::write_characteristic(
    const std::string &char_path,
    const uint8_t *data,
    size_t len,
    bool with_resp
) {
  if (!lazy_init()) {
    return false;
  }
  return BackendBluez::instance().write_characteristic(char_path, data, len, with_resp);
}

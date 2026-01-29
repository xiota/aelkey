#include "device_backend_gatt.h"

#include <cstring>
#include <iostream>
#include <string>
#include <string_view>

#include <dbus/dbus.h>
#include <sol/sol.hpp>
#include <sys/epoll.h>

#include "aelkey_state.h"
#include "device_backend_gatt.h"
#include "device_helpers.h"
#include "dispatcher_gatt.h"

bool DeviceBackendGATT::on_init() {
  if (conn_) {
    return true;
  }

  conn_ = dbus_bus_get(DBUS_BUS_SYSTEM, nullptr);
  if (!conn_) {
    return false;
  }

  dbus_connection_set_exit_on_disconnect(conn_, false);

  if (!dbus_connection_get_unix_fd(conn_, &fd_)) {
    fd_ = -1;
    conn_ = nullptr;
    return false;
  }

  return DispatcherGATT::instance().lazy_init();
}

void DeviceBackendGATT::pump_messages() {
  lazy_init();
  if (!conn_) {
    return;
  }

  // Non-blocking read
  dbus_connection_read_write(conn_, 0);

  // Process ALL pending messages
  while (true) {
    DBusMessage *msg = dbus_connection_pop_message(conn_);
    if (!msg) {
      break;
    }

    process_one_message(msg);
    dbus_message_unref(msg);
  }
}

void DeviceBackendGATT::process_one_message(DBusMessage *msg) {
  sol::state_view lua(AelkeyState::instance().lua_vm);

  const char *path = dbus_message_get_path(msg);
  if (!path) {
    return;
  }

  std::vector<uint8_t> bytes;

  DBusMessageIter args;
  dbus_message_iter_init(msg, &args);

  const char *iface = nullptr;
  dbus_message_iter_get_basic(&args, &iface);

  if (!iface || strcmp(iface, "org.bluez.GattCharacteristic1") != 0) {
    return;
  }

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

  auto &state = AelkeyState::instance();
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

void DeviceBackendGATT::start_notify(const std::string &char_path) {
  DBusMessage *msg = dbus_message_new_method_call(
      "org.bluez", char_path.c_str(), "org.bluez.GattCharacteristic1", "StartNotify"
  );

  DBusMessage *reply = dbus_connection_send_with_reply_and_block(conn_, msg, -1, nullptr);
  dbus_message_unref(msg);
  if (reply) {
    dbus_message_unref(reply);
  }
}

void DeviceBackendGATT::stop_notify(const std::string &char_path) {
  DBusMessage *msg = dbus_message_new_method_call(
      "org.bluez", char_path.c_str(), "org.bluez.GattCharacteristic1", "StopNotify"
  );

  DBusMessage *reply = dbus_connection_send_with_reply_and_block(conn_, msg, -1, nullptr);
  dbus_message_unref(msg);
  if (reply) {
    dbus_message_unref(reply);
  }
}

GattPathType DeviceBackendGATT::classify_gatt_path(const std::string &path) {
  if (path.find("/char") != std::string::npos) {
    return GattPathType::Characteristic;
  }
  if (path.find("/service") != std::string::npos) {
    return GattPathType::Service;
  }
  return GattPathType::Device;
}

std::string DeviceBackendGATT::derive_device_path_from_char_path(const std::string &char_path) {
  std::string prefix = "/service";
  size_t pos = char_path.find(prefix);
  if (pos == std::string::npos) {
    return {};
  }
  return char_path.substr(0, pos);
}

DBusMessage *DeviceBackendGATT::get_managed_objects() {
  DBusMessage *msg = dbus_message_new_method_call(
      "org.bluez", "/", "org.freedesktop.DBus.ObjectManager", "GetManagedObjects"
  );

  DBusMessage *resp = dbus_connection_send_with_reply_and_block(conn_, msg, -1, nullptr);
  dbus_message_unref(msg);
  return resp;
}

std::string DeviceBackendGATT::get_characteristic_uuid(const std::string &path) {
  DBusMessage *msg = get_managed_objects();
  if (!msg) {
    return "";
  }

  DBusMessageIter it;
  dbus_message_iter_init(msg, &it);

  if (dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_ARRAY) {
    dbus_message_unref(msg);
    return "";
  }

  DBusMessageIter dict;
  dbus_message_iter_recurse(&it, &dict);

  while (dbus_message_iter_get_arg_type(&dict) == DBUS_TYPE_DICT_ENTRY) {
    DBusMessageIter entry;
    dbus_message_iter_recurse(&dict, &entry);

    const char *object_path = nullptr;
    dbus_message_iter_get_basic(&entry, &object_path);

    dbus_message_iter_next(&entry);

    if (dbus_message_iter_get_arg_type(&entry) != DBUS_TYPE_ARRAY) {
      dbus_message_iter_next(&dict);
      continue;
    }

    DBusMessageIter iface_dict;
    dbus_message_iter_recurse(&entry, &iface_dict);

    if (object_path && path == object_path) {
      while (dbus_message_iter_get_arg_type(&iface_dict) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter iface_entry;
        dbus_message_iter_recurse(&iface_dict, &iface_entry);

        const char *iface_name = nullptr;
        dbus_message_iter_get_basic(&iface_entry, &iface_name);

        dbus_message_iter_next(&iface_entry);

        if (!iface_name || strcmp(iface_name, "org.bluez.GattCharacteristic1") != 0) {
          dbus_message_iter_next(&iface_dict);
          continue;
        }

        if (dbus_message_iter_get_arg_type(&iface_entry) != DBUS_TYPE_ARRAY) {
          dbus_message_iter_next(&iface_dict);
          continue;
        }

        DBusMessageIter props_dict;
        dbus_message_iter_recurse(&iface_entry, &props_dict);

        while (dbus_message_iter_get_arg_type(&props_dict) == DBUS_TYPE_DICT_ENTRY) {
          DBusMessageIter prop_entry;
          dbus_message_iter_recurse(&props_dict, &prop_entry);

          const char *prop_name = nullptr;
          dbus_message_iter_get_basic(&prop_entry, &prop_name);

          dbus_message_iter_next(&prop_entry);

          if (!prop_name || strcmp(prop_name, "UUID") != 0) {
            dbus_message_iter_next(&props_dict);
            continue;
          }

          if (dbus_message_iter_get_arg_type(&prop_entry) != DBUS_TYPE_VARIANT) {
            dbus_message_iter_next(&props_dict);
            continue;
          }

          DBusMessageIter variant;
          dbus_message_iter_recurse(&prop_entry, &variant);

          const char *uuid = nullptr;
          dbus_message_iter_get_basic(&variant, &uuid);

          std::string result = uuid ? uuid : "";
          dbus_message_unref(msg);
          return result;
        }

        dbus_message_iter_next(&iface_dict);
      }
    }

    dbus_message_iter_next(&dict);
  }

  dbus_message_unref(msg);
  return "";
}

std::vector<std::string> DeviceBackendGATT::get_characteristic_flags(const std::string &path) {
  std::vector<std::string> out;

  DBusMessage *msg = get_managed_objects();
  if (!msg) {
    return out;
  }

  DBusMessageIter it, dict;
  dbus_message_iter_init(msg, &it);
  dbus_message_iter_recurse(&it, &dict);

  while (dbus_message_iter_get_arg_type(&dict) != DBUS_TYPE_INVALID) {
    DBusMessageIter entry, iface_dict;
    const char *object_path = nullptr;

    dbus_message_iter_recurse(&dict, &entry);
    dbus_message_iter_get_basic(&entry, &object_path);
    dbus_message_iter_next(&entry);
    dbus_message_iter_recurse(&entry, &iface_dict);

    if (object_path && path == object_path) {
      while (dbus_message_iter_get_arg_type(&iface_dict) != DBUS_TYPE_INVALID) {
        DBusMessageIter props_dict;
        const char *iface_name = nullptr;

        dbus_message_iter_recurse(&iface_dict, &props_dict);
        dbus_message_iter_get_basic(&props_dict, &iface_name);
        dbus_message_iter_next(&props_dict);

        if (iface_name && strcmp(iface_name, "org.bluez.GattCharacteristic1") == 0) {
          DBusMessageIter prop_entry;
          dbus_message_iter_recurse(&props_dict, &prop_entry);

          while (dbus_message_iter_get_arg_type(&prop_entry) != DBUS_TYPE_INVALID) {
            DBusMessageIter prop, variant;
            const char *prop_name = nullptr;

            dbus_message_iter_recurse(&prop_entry, &prop);
            dbus_message_iter_get_basic(&prop, &prop_name);
            dbus_message_iter_next(&prop);
            dbus_message_iter_recurse(&prop, &variant);

            if (prop_name && strcmp(prop_name, "Flags") == 0) {
              DBusMessageIter array;
              dbus_message_iter_recurse(&variant, &array);

              while (dbus_message_iter_get_arg_type(&array) == DBUS_TYPE_STRING) {
                const char *flag = nullptr;
                dbus_message_iter_get_basic(&array, &flag);
                if (flag) {
                  out.emplace_back(flag);
                }
                dbus_message_iter_next(&array);
              }

              dbus_message_unref(msg);
              return out;
            }

            dbus_message_iter_next(&prop_entry);
          }
        }

        dbus_message_iter_next(&iface_dict);
      }
    }

    dbus_message_iter_next(&dict);
  }

  dbus_message_unref(msg);
  return out;
}

void DeviceBackendGATT::print_characteristic_inspect_line(const std::string &ch) {
  // Extract service and characteristic handles from DBus path
  std::string service_hex = "0000";
  std::string char_hex = "0000";

  auto svc_pos = ch.find("service");
  if (svc_pos != std::string::npos) {
    service_hex = ch.substr(svc_pos + 7, 4);
  }

  auto chr_pos = ch.find("char");
  if (chr_pos != std::string::npos) {
    char_hex = ch.substr(chr_pos + 4, 4);
  }

  // Get UUID and flags using your existing helpers
  std::string uuid = get_characteristic_uuid(ch);
  std::vector<std::string> flags = get_characteristic_flags(ch);

  // Trim UUID to last 4 hex digits if it's a 128‑bit UUID
  if (uuid.size() >= 4) {
    uuid = uuid.substr(uuid.size() - 4);
  }

  // Format flags
  std::ostringstream fl;
  fl << "[";
  for (size_t i = 0; i < flags.size(); i++) {
    fl << flags[i];
    if (i + 1 < flags.size()) {
      fl << ", ";
    }
  }
  fl << "]";

  // Final output line
  std::cout << "-- service=0x" << service_hex << ", char=0x" << char_hex << ", -- uuid=" << uuid
            << ", flags=" << fl.str() << std::endl;
}

bool DeviceBackendGATT::characteristic_supports_notify(const std::string &char_path) {
  DBusMessage *msg;
  DBusMessage *reply;
  DBusMessageIter args;

  msg = dbus_message_new_method_call(
      "org.bluez", char_path.c_str(), "org.freedesktop.DBus.Properties", "Get"
  );

  const char *iface = "org.bluez.GattCharacteristic1";
  const char *prop = "Flags";

  dbus_message_append_args(
      msg, DBUS_TYPE_STRING, &iface, DBUS_TYPE_STRING, &prop, DBUS_TYPE_INVALID
  );

  reply = dbus_connection_send_with_reply_and_block(conn_, msg, -1, nullptr);
  dbus_message_unref(msg);

  if (!reply) {
    return false;
  }

  dbus_message_iter_init(reply, &args);

  // Flags is an array of strings
  DBusMessageIter variant, array;
  dbus_message_iter_recurse(&args, &variant);
  dbus_message_iter_recurse(&variant, &array);

  bool supports = false;

  while (dbus_message_iter_get_arg_type(&array) == DBUS_TYPE_STRING) {
    const char *flag;
    dbus_message_iter_get_basic(&array, &flag);

    if (strcmp(flag, "notify") == 0) {
      supports = true;
      break;
    }

    dbus_message_iter_next(&array);
  }

  dbus_message_unref(reply);
  return supports;
}

std::string DeviceBackendGATT::resolve_gatt_paths(
    const InputDecl &decl,
    std::vector<std::string> *found_characteristics
) {
  DBusMessage *resp = get_managed_objects();
  if (!resp) {
    return {};
  }

  DBusMessageIter iter, dict;

  dbus_message_iter_init(resp, &iter);
  dbus_message_iter_recurse(&iter, &dict);

  auto devices = get_matching_devices(decl, dict);
  if (devices.empty()) {
    dbus_message_unref(resp);
    return {};
  }

  if (!decl.service && !found_characteristics) {
    dbus_message_unref(resp);
    return devices[0];
  }

  dbus_message_iter_init(resp, &iter);
  dbus_message_iter_recurse(&iter, &dict);

  auto services = get_matching_services(decl, devices, dict);
  if (services.empty()) {
    dbus_message_unref(resp);
    return {};
  }

  if (!decl.characteristic && !found_characteristics) {
    dbus_message_unref(resp);
    return services[0];
  }

  dbus_message_iter_init(resp, &iter);
  dbus_message_iter_recurse(&iter, &dict);

  auto characteristics = get_matching_characteristics(decl, services, dict);

  if (found_characteristics) {
    *found_characteristics = characteristics;
  }

  dbus_message_unref(resp);

  if (!decl.service) {
    return devices[0];
  }

  if (!decl.characteristic) {
    return services[0];
  }

  if (characteristics.empty()) {
    std::fprintf(stderr, "GATT match: no matching characteristic found\n");
    return {};
  }

  return characteristics[0];
}

std::vector<std::string>
DeviceBackendGATT::get_matching_devices(const InputDecl &decl, DBusMessageIter &array) {
  std::vector<std::string> result;

  DBusMessageIter it = array;

  while (dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_INVALID) {
    DBusMessageIter entry, iface_dict;
    const char *object_path = nullptr;

    dbus_message_iter_recurse(&it, &entry);
    dbus_message_iter_get_basic(&entry, &object_path);
    dbus_message_iter_next(&entry);
    dbus_message_iter_recurse(&entry, &iface_dict);

    bool is_device = false;
    std::string name, alias, address;

    while (dbus_message_iter_get_arg_type(&iface_dict) != DBUS_TYPE_INVALID) {
      DBusMessageIter props;
      const char *iface_name = nullptr;

      dbus_message_iter_recurse(&iface_dict, &props);
      dbus_message_iter_get_basic(&props, &iface_name);
      dbus_message_iter_next(&props);

      if (iface_name && strcmp(iface_name, "org.bluez.Device1") == 0) {
        is_device = true;

        DBusMessageIter prop_dict;
        dbus_message_iter_recurse(&props, &prop_dict);

        while (dbus_message_iter_get_arg_type(&prop_dict) != DBUS_TYPE_INVALID) {
          DBusMessageIter prop_entry, var;
          const char *key = nullptr;

          dbus_message_iter_recurse(&prop_dict, &prop_entry);
          dbus_message_iter_get_basic(&prop_entry, &key);
          dbus_message_iter_next(&prop_entry);
          dbus_message_iter_recurse(&prop_entry, &var);

          if (strcmp(key, "Name") == 0) {
            const char *v = nullptr;
            dbus_message_iter_get_basic(&var, &v);
            name = v ? v : "";
          } else if (strcmp(key, "Alias") == 0) {
            const char *v = nullptr;
            dbus_message_iter_get_basic(&var, &v);
            alias = v ? v : "";
          } else if (strcmp(key, "Address") == 0) {
            const char *v = nullptr;
            dbus_message_iter_get_basic(&var, &v);
            address = v ? v : "";
          }

          dbus_message_iter_next(&prop_dict);
        }
      }

      dbus_message_iter_next(&iface_dict);
    }

    if (is_device) {
      bool match = false;

      // Match uniq (Bluetooth MAC address)
      if (!decl.uniq.empty() && match_string(decl.uniq, address)) {
        match = true;
      }

      // Match name or alias
      if (!match && !decl.name.empty() &&
          (match_string(decl.name, name) || match_string(decl.name, alias))) {
        match = true;
      }

      if (match) {
        result.push_back(object_path);
      }
    }

    dbus_message_iter_next(&it);
  }

  return result;
}

std::vector<std::string> DeviceBackendGATT::get_matching_services(
    const InputDecl &decl,
    const std::vector<std::string> &candidate_devices,
    DBusMessageIter &array
) {
  std::vector<std::string> result;

  for (const auto &dev_path : candidate_devices) {
    DBusMessageIter it = array;

    while (dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_INVALID) {
      DBusMessageIter entry, iface_dict;
      const char *object_path = nullptr;

      dbus_message_iter_recurse(&it, &entry);
      dbus_message_iter_get_basic(&entry, &object_path);
      dbus_message_iter_next(&entry);
      dbus_message_iter_recurse(&entry, &iface_dict);

      while (dbus_message_iter_get_arg_type(&iface_dict) != DBUS_TYPE_INVALID) {
        DBusMessageIter props;
        const char *iface_name = nullptr;

        dbus_message_iter_recurse(&iface_dict, &props);
        dbus_message_iter_get_basic(&props, &iface_name);
        dbus_message_iter_next(&props);

        if (iface_name && strcmp(iface_name, "org.bluez.GattService1") == 0 &&
            strstr(object_path, dev_path.c_str()) == object_path) {
          const char *p = strstr(object_path, "service");
          if (p) {
            int handle = strtoul(p + 7, nullptr, 16);
            if (decl.service == 0) {
              // No specific service requested → return all services under the device
              result.push_back(object_path);
            } else {
              // Match specific service handle
              if (handle == decl.service) {
                result.push_back(object_path);
              }
            }
          }
        }

        dbus_message_iter_next(&iface_dict);
      }

      dbus_message_iter_next(&it);
    }
  }

  return result;
}

std::vector<std::string> DeviceBackendGATT::get_matching_characteristics(
    const InputDecl &decl,
    const std::vector<std::string> &candidate_services,
    DBusMessageIter &array
) {
  std::vector<std::string> result;

  for (const auto &svc_path : candidate_services) {
    DBusMessageIter it = array;  // fresh scan for each service

    while (dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_INVALID) {
      DBusMessageIter entry, iface_dict;
      const char *object_path = nullptr;

      dbus_message_iter_recurse(&it, &entry);
      dbus_message_iter_get_basic(&entry, &object_path);
      dbus_message_iter_next(&entry);
      dbus_message_iter_recurse(&entry, &iface_dict);

      while (dbus_message_iter_get_arg_type(&iface_dict) != DBUS_TYPE_INVALID) {
        DBusMessageIter props;
        const char *iface_name = nullptr;

        dbus_message_iter_recurse(&iface_dict, &props);
        dbus_message_iter_get_basic(&props, &iface_name);
        dbus_message_iter_next(&props);

        if (iface_name && strcmp(iface_name, "org.bluez.GattCharacteristic1") == 0 &&
            strstr(object_path, svc_path.c_str()) == object_path) {
          const char *p = strstr(object_path, "char");
          if (p) {
            int handle = strtoul(p + 4, nullptr, 16);
            if (decl.characteristic == 0) {
              // No specific characteristic requested → collect all
              result.push_back(object_path);
            } else {
              if (handle == decl.characteristic) {
                result.push_back(object_path);
              }
            }
          }
        }

        dbus_message_iter_next(&iface_dict);
      }

      dbus_message_iter_next(&it);
    }
  }

  return result;
}

bool DeviceBackendGATT::read_characteristic(
    const std::string &char_path,
    std::vector<uint8_t> &out_data
) {
  out_data.clear();

  lazy_init();
  if (!conn_) {
    return false;
  }

  DBusMessage *msg = dbus_message_new_method_call(
      "org.bluez", char_path.c_str(), "org.bluez.GattCharacteristic1", "ReadValue"
  );

  // Append empty a{sv} options
  DBusMessageIter args;
  dbus_message_iter_init_append(msg, &args);
  DBusMessageIter dict;
  dbus_message_iter_open_container(&args, DBUS_TYPE_ARRAY, "{sv}", &dict);
  dbus_message_iter_close_container(&args, &dict);

  DBusMessage *reply = dbus_connection_send_with_reply_and_block(conn_, msg, -1, nullptr);
  dbus_message_unref(msg);

  if (!reply) {
    return false;
  }

  DBusMessageIter iter;
  dbus_message_iter_init(reply, &iter);

  // Reply is: ay
  if (dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_ARRAY) {
    DBusMessageIter array;
    dbus_message_iter_recurse(&iter, &array);

    while (dbus_message_iter_get_arg_type(&array) == DBUS_TYPE_BYTE) {
      uint8_t b;
      dbus_message_iter_get_basic(&array, &b);
      out_data.push_back(b);
      dbus_message_iter_next(&array);
    }
  }

  dbus_message_unref(reply);
  return true;
}

bool DeviceBackendGATT::write_characteristic(
    const std::string &char_path,
    const uint8_t *data,
    size_t len,
    bool with_resp
) {
  lazy_init();
  if (!conn_) {
    return false;
  }

  DBusMessage *msg = dbus_message_new_method_call(
      "org.bluez", char_path.c_str(), "org.bluez.GattCharacteristic1", "WriteValue"
  );

  DBusMessageIter args;
  dbus_message_iter_init_append(msg, &args);

  // First argument: ay (byte array)
  DBusMessageIter array;
  dbus_message_iter_open_container(&args, DBUS_TYPE_ARRAY, "y", &array);
  for (size_t i = 0; i < len; ++i) {
    uint8_t b = data[i];
    dbus_message_iter_append_basic(&array, DBUS_TYPE_BYTE, &b);
  }
  dbus_message_iter_close_container(&args, &array);

  // Second argument: a{sv} options
  DBusMessageIter opts;
  dbus_message_iter_open_container(&args, DBUS_TYPE_ARRAY, "{sv}", &opts);

  if (with_resp) {
    // { "type": <"request"> }
    DBusMessageIter dict_entry;
    dbus_message_iter_open_container(&opts, DBUS_TYPE_DICT_ENTRY, nullptr, &dict_entry);

    const char *key = "type";
    dbus_message_iter_append_basic(&dict_entry, DBUS_TYPE_STRING, &key);

    DBusMessageIter variant;
    dbus_message_iter_open_container(&dict_entry, DBUS_TYPE_VARIANT, "s", &variant);

    const char *val = "request";
    dbus_message_iter_append_basic(&variant, DBUS_TYPE_STRING, &val);

    dbus_message_iter_close_container(&dict_entry, &variant);
    dbus_message_iter_close_container(&opts, &dict_entry);
  }

  dbus_message_iter_close_container(&args, &opts);

  DBusMessage *reply = dbus_connection_send_with_reply_and_block(conn_, msg, -1, nullptr);
  dbus_message_unref(msg);

  if (!reply) {
    return false;
  }

  dbus_message_unref(reply);
  return true;
}

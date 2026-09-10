#include "backend_bluez.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <dbus/dbus.h>

#include "device_declarations.h"
#include "utils/regex_match.h"

BackendBluez::~BackendBluez() {
  if (conn_) {
    dbus_connection_unref(conn_);
    conn_ = nullptr;
  }
}

bool BackendBluez::on_init() {
  return ensure_client();
}

bool BackendBluez::ensure_client() {
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

  return true;
}

void BackendBluez::add_match_rule(const std::string &rule) {
  if (!ensure_client()) {
    return;
  }
  dbus_bus_add_match(conn_, rule.c_str(), nullptr);
  dbus_connection_flush(conn_);
}

void BackendBluez::start_notify(const std::string &char_path) {
  if (!ensure_client()) {
    return;
  }

  DBusMessage *msg = dbus_message_new_method_call(
      "org.bluez", char_path.c_str(), "org.bluez.GattCharacteristic1", "StartNotify"
  );

  DBusMessage *reply = dbus_connection_send_with_reply_and_block(conn_, msg, -1, nullptr);
  dbus_message_unref(msg);
  if (reply) {
    dbus_message_unref(reply);
  }
}

void BackendBluez::stop_notify(const std::string &char_path) {
  if (!ensure_client()) {
    return;
  }

  DBusMessage *msg = dbus_message_new_method_call(
      "org.bluez", char_path.c_str(), "org.bluez.GattCharacteristic1", "StopNotify"
  );

  DBusMessage *reply = dbus_connection_send_with_reply_and_block(conn_, msg, -1, nullptr);
  dbus_message_unref(msg);
  if (reply) {
    dbus_message_unref(reply);
  }
}

GattPathType BackendBluez::classify_gatt_path(const std::string &path) {
  if (path.find("/char") != std::string::npos) {
    return GattPathType::Characteristic;
  }
  if (path.find("/service") != std::string::npos) {
    return GattPathType::Service;
  }
  return GattPathType::Device;
}

std::string BackendBluez::derive_device_path_from_char_path(const std::string &char_path) {
  std::string prefix = "/service";
  size_t pos = char_path.find(prefix);
  if (pos == std::string::npos) {
    return {};
  }
  return char_path.substr(0, pos);
}

DBusMessage *BackendBluez::get_managed_objects() {
  if (!ensure_client()) {
    return nullptr;
  }

  DBusMessage *msg = dbus_message_new_method_call(
      "org.bluez", "/", "org.freedesktop.DBus.ObjectManager", "GetManagedObjects"
  );

  DBusMessage *resp = dbus_connection_send_with_reply_and_block(conn_, msg, -1, nullptr);
  dbus_message_unref(msg);
  return resp;
}

std::string BackendBluez::get_characteristic_uuid(const std::string &path) {
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

std::vector<std::string> BackendBluez::get_characteristic_flags(const std::string &path) {
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

void BackendBluez::print_characteristic_inspect_line(const std::string &ch) {
  std::string service_hex = "0000";
  std::string char_hex = "0000";

  auto svc_pos = ch.find("service");
  if (svc_pos != std::string::npos && svc_pos + 7 <= ch.size()) {
    service_hex = ch.substr(svc_pos + 7, 4);
  }

  auto chr_pos = ch.find("char");
  if (chr_pos != std::string::npos && chr_pos + 4 <= ch.size()) {
    char_hex = ch.substr(chr_pos + 4, 4);
  }

  std::string uuid = get_characteristic_uuid(ch);
  std::vector<std::string> flags = get_characteristic_flags(ch);

  if (uuid.size() >= 4) {
    uuid = uuid.substr(uuid.size() - 4);
  }

  std::string flags_str = "[";
  for (size_t i = 0; i < flags.size(); ++i) {
    flags_str += flags[i];
    if (i + 1 < flags.size()) {
      flags_str += ", ";
    }
  }
  flags_str += "]";

  std::printf(
      "-- service=0x%s, char=0x%s, uuid=%s, flags=%s\n",
      service_hex.c_str(),
      char_hex.c_str(),
      uuid.c_str(),
      flags_str.c_str()
  );
}

bool BackendBluez::characteristic_supports_notify(const std::string &char_path) {
  if (!ensure_client()) {
    return false;
  }

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

std::string BackendBluez::resolve_gatt_paths(
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

  if (decl.services.empty() && !found_characteristics) {
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

  if (decl.characteristics.empty() && !found_characteristics) {
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

  if (decl.services.empty()) {
    return devices[0];
  }

  if (decl.characteristics.empty()) {
    return services[0];
  }

  if (characteristics.empty()) {
    std::fprintf(stderr, "GATT match: no matching characteristic found\n");
    return {};
  }

  return characteristics[0];
}

std::vector<std::string>
BackendBluez::get_matching_devices(const InputDecl &decl, DBusMessageIter &array) {
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

      if (!decl.uniq.empty() && AelkeyUtil::match_string(decl.uniq, address)) {
        match = true;
      }

      if (!match && !decl.name.empty() &&
          (AelkeyUtil::match_string(decl.name, name) ||
           AelkeyUtil::match_string(decl.name, alias))) {
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

std::vector<std::string> BackendBluez::get_matching_services(
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
            if (decl.services.empty()) {
              result.push_back(object_path);
            } else {
              if (std::find(decl.services.begin(), decl.services.end(), handle) !=
                  decl.services.end()) {
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

std::vector<std::string> BackendBluez::get_matching_characteristics(
    const InputDecl &decl,
    const std::vector<std::string> &candidate_services,
    DBusMessageIter &array
) {
  std::vector<std::string> result;

  for (const auto &svc_path : candidate_services) {
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

        if (iface_name && strcmp(iface_name, "org.bluez.GattCharacteristic1") == 0 &&
            strstr(object_path, svc_path.c_str()) == object_path) {
          const char *p = strstr(object_path, "char");
          if (p) {
            int handle = strtoul(p + 4, nullptr, 16);
            if (decl.characteristics.empty()) {
              result.push_back(object_path);
            } else {
              if (std::find(decl.characteristics.begin(), decl.characteristics.end(), handle) !=
                  decl.characteristics.end()) {
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

bool BackendBluez::read_characteristic(
    const std::string &char_path,
    std::vector<uint8_t> &out_data
) {
  out_data.clear();

  if (!ensure_client()) {
    return false;
  }

  DBusMessage *msg = dbus_message_new_method_call(
      "org.bluez", char_path.c_str(), "org.bluez.GattCharacteristic1", "ReadValue"
  );

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

bool BackendBluez::write_characteristic(
    const std::string &char_path,
    const uint8_t *data,
    size_t len,
    bool with_resp
) {
  if (!ensure_client()) {
    return false;
  }

  DBusMessage *msg = dbus_message_new_method_call(
      "org.bluez", char_path.c_str(), "org.bluez.GattCharacteristic1", "WriteValue"
  );

  DBusMessageIter args;
  dbus_message_iter_init_append(msg, &args);

  DBusMessageIter array;
  dbus_message_iter_open_container(&args, DBUS_TYPE_ARRAY, "y", &array);
  for (size_t i = 0; i < len; ++i) {
    uint8_t b = data[i];
    dbus_message_iter_append_basic(&array, DBUS_TYPE_BYTE, &b);
  }
  dbus_message_iter_close_container(&args, &array);

  DBusMessageIter opts;
  dbus_message_iter_open_container(&args, DBUS_TYPE_ARRAY, "{sv}", &opts);

  if (with_resp) {
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

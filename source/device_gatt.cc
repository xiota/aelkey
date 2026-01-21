#include "device_gatt.h"

#include <cstring>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include <dbus/dbus.h>
#include <sol/sol.hpp>
#include <sys/epoll.h>

#include "aelkey_state.h"
#include "device_helpers.h"

static void start_notify(DBusConnection *conn, const std::string &char_path) {
  DBusMessage *msg = dbus_message_new_method_call(
      "org.bluez", char_path.c_str(), "org.bluez.GattCharacteristic1", "StartNotify"
  );

  DBusMessage *reply = dbus_connection_send_with_reply_and_block(conn, msg, -1, nullptr);
  dbus_message_unref(msg);
  if (reply) {
    dbus_message_unref(reply);
  }
}

static void stop_notify(DBusConnection *conn, const std::string &char_path) {
  DBusMessage *msg = dbus_message_new_method_call(
      "org.bluez", char_path.c_str(), "org.bluez.GattCharacteristic1", "StopNotify"
  );

  DBusMessage *reply = dbus_connection_send_with_reply_and_block(conn, msg, -1, nullptr);
  dbus_message_unref(msg);
  if (reply) {
    dbus_message_unref(reply);
  }
}

void ensure_gatt_initialized() {
  auto &state = AelkeyState::instance();
  if (state.g_dbus_conn) {
    return;
  }

  state.g_dbus_conn = dbus_bus_get(DBUS_BUS_SYSTEM, nullptr);
  if (!state.g_dbus_conn) {
    std::fprintf(stderr, "GATT: failed to connect to system D-Bus\n");
    return;
  }

  dbus_connection_set_exit_on_disconnect(state.g_dbus_conn, false);

  if (!dbus_connection_get_unix_fd(state.g_dbus_conn, &state.g_dbus_fd)) {
    std::fprintf(stderr, "GATT: failed to get D-Bus fd\n");
    return;
  }

  struct epoll_event ev{};
  ev.events = EPOLLIN;
  ev.data.fd = state.g_dbus_fd;
  if (epoll_ctl(state.epfd, EPOLL_CTL_ADD, state.g_dbus_fd, &ev) != 0) {
    std::fprintf(stderr, "GATT: epoll_ctl failed for D-Bus fd\n");
  }
}

// Helper: derive BlueZ device path from characteristic path.
// Example: /org/bluez/hci0/dev_XX/service0010/char002A
//   → /org/bluez/hci0/dev_XX
static std::string derive_device_path_from_char_path(const std::string &char_path) {
  std::string prefix = "/service";
  size_t pos = char_path.find(prefix);
  if (pos == std::string::npos) {
    return {};
  }
  return char_path.substr(0, pos);
}

enum class GattPathType { Device, Service, Characteristic };

static GattPathType classify_gatt_path(const std::string &path) {
  if (path.find("/char") != std::string::npos) {
    return GattPathType::Characteristic;
  }
  if (path.find("/service") != std::string::npos) {
    return GattPathType::Service;
  }
  return GattPathType::Device;
}

static DBusMessage *get_managed_objects() {
  auto &state = AelkeyState::instance();
  DBusMessage *msg = dbus_message_new_method_call(
      "org.bluez", "/", "org.freedesktop.DBus.ObjectManager", "GetManagedObjects"
  );

  DBusMessage *resp =
      dbus_connection_send_with_reply_and_block(state.g_dbus_conn, msg, -1, nullptr);

  dbus_message_unref(msg);
  return resp;
}

static std::string get_characteristic_uuid(const std::string &path) {
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

static std::vector<std::string> get_characteristic_flags(const std::string &path) {
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

static void print_characteristic_inspect_line(const std::string &ch) {
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

bool characteristic_supports_notify(DBusConnection *conn, const std::string &char_path) {
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

  reply = dbus_connection_send_with_reply_and_block(conn, msg, -1, nullptr);
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

InputCtx attach_gatt_device(const InputDecl &decl) {
  InputCtx ctx;
  ctx.decl = decl;

  auto &state = AelkeyState::instance();
  DBusConnection *conn = state.g_dbus_conn;
  if (!conn) {
    std::fprintf(stderr, "GATT: no D-Bus connection\n");
    return ctx;
  }

  if (decl.devnode.empty()) {
    std::fprintf(stderr, "GATT: no GATT path in devnode for %s\n", decl.id.c_str());
    return ctx;
  }

  // Determine what kind of GATT path this is
  GattPathType type = classify_gatt_path(decl.devnode);

  // Determine the device root path
  if (type == GattPathType::Characteristic) {
    ctx.gatt_path = derive_device_path_from_char_path(decl.devnode);
    if (ctx.gatt_path.empty()) {
      std::fprintf(
          stderr, "GATT: failed to derive device path from %s\n", decl.devnode.c_str()
      );
    }
  } else {
    // For device-level or service-level matches, the path itself is the root
    ctx.gatt_path = decl.devnode;
  }

  if (type != GattPathType::Characteristic) {
    std::vector<std::string> found_characteristics;
    match_gatt_device(decl, &found_characteristics);

    for (const auto &ch : found_characteristics) {
      // Always print inspect info
      print_characteristic_inspect_line(ch);

      // Only subscribe if characteristic supports notify
      if (characteristic_supports_notify(conn, ch)) {
        std::string rule =
            "type='signal',interface='org.freedesktop.DBus.Properties',"
            "member='PropertiesChanged',path='" +
            ch + "'";

        dbus_bus_add_match(conn, rule.c_str(), nullptr);

        start_notify(conn, ch);
      }
    }

    dbus_connection_flush(conn);
  } else {
    print_characteristic_inspect_line(decl.devnode);

    // Notify
    std::string rule =
        "type='signal',interface='org.freedesktop.DBus.Properties',"
        "member='PropertiesChanged',path='" +
        decl.devnode + "'";

    dbus_bus_add_match(conn, rule.c_str(), nullptr);
    dbus_connection_flush(conn);

    start_notify(conn, decl.devnode);
  }

  ctx.active = true;
  return ctx;
}

void detach_gatt_device(InputCtx &ctx) {
  auto &state = AelkeyState::instance();
  DBusConnection *conn = state.g_dbus_conn;
  if (!conn) {
    return;
  }

  if (!ctx.decl.devnode.empty()) {
    stop_notify(conn, ctx.decl.devnode);
  }

  ctx.active = false;
  std::fprintf(stderr, "Detached GATT characteristic: %s\n", ctx.decl.devnode.c_str());
}

static void process_one_gatt_message(sol::this_state ts, DBusMessage *msg) {
  sol::state_view lua(ts);

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

  // Route to correct InputCtx
  auto &state = AelkeyState::instance();
  for (auto &kv : state.input_map) {
    InputCtx &ctx = kv.second;

    if (ctx.decl.type != "gatt") {
      continue;
    }

    if (!ctx.decl.on_event.empty()) {
      sol::object obj = lua[ctx.decl.on_event];
      if (!obj.is<sol::function>()) {
        continue;
      }

      sol::function cb = obj.as<sol::function>();

      sol::table tbl = lua.create_table();
      tbl["device"] = ctx.decl.id;
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

void dispatch_gatt(sol::this_state ts) {
  auto &state = AelkeyState::instance();
  DBusConnection *conn = state.g_dbus_conn;
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

    process_one_gatt_message(ts, msg);
    dbus_message_unref(msg);
  }
}

// ---------------------------------------------------------------------
// Device matching
// ---------------------------------------------------------------------

static std::vector<std::string>
get_matching_devices(const InputDecl &decl, DBusMessageIter &array) {
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

static std::vector<std::string> get_matching_services(
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

static std::vector<std::string> get_matching_characteristic(
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

std::string
match_gatt_device(const InputDecl &decl, std::vector<std::string> *found_characteristics) {
  ensure_gatt_initialized();

  DBusMessage *resp = get_managed_objects();
  if (!resp) {
    return {};
  }

  DBusMessageIter iter, dict;

  // 1. devices
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

  // 2. services
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

  // 3. characteristics
  dbus_message_iter_init(resp, &iter);
  dbus_message_iter_recurse(&iter, &dict);

  auto characteristics = get_matching_characteristic(decl, services, dict);

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

// ---------------------------------------------------------------------
// Low-level GATT read/write helpers
// ---------------------------------------------------------------------

bool gatt_read_characteristic(const std::string &char_path, std::vector<uint8_t> &out_data) {
  out_data.clear();

  auto &state = AelkeyState::instance();
  DBusConnection *conn = state.g_dbus_conn;
  if (!conn) {
    std::fprintf(stderr, "GATT read: no D-Bus connection\n");
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

  DBusMessage *reply = dbus_connection_send_with_reply_and_block(conn, msg, -1, nullptr);
  dbus_message_unref(msg);

  if (!reply) {
    std::fprintf(stderr, "GATT read: ReadValue failed\n");
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

bool gatt_write_characteristic(
    const std::string &char_path,
    const uint8_t *data,
    size_t len,
    bool with_resp
) {
  auto &state = AelkeyState::instance();
  DBusConnection *conn = state.g_dbus_conn;
  if (!conn) {
    std::fprintf(stderr, "GATT write: no D-Bus connection\n");
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

  DBusMessage *reply = dbus_connection_send_with_reply_and_block(conn, msg, -1, nullptr);
  dbus_message_unref(msg);

  if (!reply) {
    std::fprintf(stderr, "GATT write: WriteValue failed\n");
    return false;
  }

  dbus_message_unref(reply);
  return true;
}

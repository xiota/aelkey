#include "device_gatt.h"

#include <cstring>
#include <format>
#include <iostream>
#include <string>
#include <vector>

#include <dbus/dbus.h>
#include <sys/epoll.h>

#include "aelkey_state.h"
#include "luacompat.h"

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
  if (aelkey_state.g_dbus_conn) {
    return;
  }

  aelkey_state.g_dbus_conn = dbus_bus_get(DBUS_BUS_SYSTEM, nullptr);
  if (!aelkey_state.g_dbus_conn) {
    std::cerr << "GATT: failed to connect to system D-Bus\n";
    return;
  }

  dbus_connection_set_exit_on_disconnect(aelkey_state.g_dbus_conn, false);

  if (!dbus_connection_get_unix_fd(aelkey_state.g_dbus_conn, &aelkey_state.g_dbus_fd)) {
    std::cerr << "GATT: failed to get D-Bus fd\n";
    return;
  }

  struct epoll_event ev{};
  ev.events = EPOLLIN;
  ev.data.fd = aelkey_state.g_dbus_fd;
  if (epoll_ctl(aelkey_state.epfd, EPOLL_CTL_ADD, aelkey_state.g_dbus_fd, &ev) != 0) {
    std::cerr << "GATT: epoll_ctl failed for D-Bus fd\n";
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

InputCtx attach_gatt_device(const InputDecl &decl) {
  InputCtx ctx;
  ctx.decl = decl;

  DBusConnection *conn = aelkey_state.g_dbus_conn;
  if (!conn) {
    std::cerr << "GATT: no D-Bus connection\n";
    return ctx;
  }

  if (decl.devnode.empty()) {
    std::cerr << "GATT: no GATT path in devnode for " << decl.id << std::endl;
    return ctx;
  }

  // Determine what kind of GATT path this is
  GattPathType type = classify_gatt_path(decl.devnode);

  // Determine the device root path
  if (type == GattPathType::Characteristic) {
    ctx.gatt_path = derive_device_path_from_char_path(decl.devnode);
    if (ctx.gatt_path.empty()) {
      std::cerr << "GATT: failed to derive device path from " << decl.devnode << std::endl;
    }
  } else {
    // For device-level or service-level matches, the path itself is the root
    ctx.gatt_path = decl.devnode;
  }

  // Install DBus match rule
  {
    DBusError err;
    dbus_error_init(&err);

    std::string rule;

    if (type == GattPathType::Characteristic) {
      // Exact characteristic path
      rule =
          "type='signal',interface='org.freedesktop.DBus.Properties',"
          "member='PropertiesChanged',path='" +
          decl.devnode + "'";
    } else {
      // Device or service: listen to everything under this subtree
      rule =
          "type='signal',interface='org.freedesktop.DBus.Properties',"
          "sender='org.bluez',path_namespace='" +
          decl.devnode + "'";
    }

    dbus_bus_add_match(conn, rule.c_str(), &err);
    dbus_connection_flush(conn);

    if (dbus_error_is_set(&err)) {
      std::cerr << "GATT: Failed to add match rule: " << err.message << std::endl;
      dbus_error_free(&err);
      return ctx;
    } else {
      std::cerr << "GATT: Added match rule for " << decl.devnode << std::endl;
    }
  }

  // Only StartNotify for characteristics
  if (type == GattPathType::Characteristic) {
    start_notify(conn, decl.devnode);
    std::cerr << "Attached GATT characteristic: " << decl.devnode << std::endl;
  } else {
    std::cerr << "Attached GATT device/service: " << decl.devnode << std::endl;
  }

  ctx.active = true;
  return ctx;
}

void detach_gatt_device(InputCtx &ctx) {
  DBusConnection *conn = aelkey_state.g_dbus_conn;
  if (!conn) {
    return;
  }

  if (!ctx.decl.devnode.empty()) {
    stop_notify(conn, ctx.decl.devnode);
  }

  ctx.active = false;
  std::cerr << "Detached GATT characteristic: " << ctx.decl.devnode << std::endl;
}

void dispatch_gatt(lua_State *L) {
  DBusConnection *conn = aelkey_state.g_dbus_conn;
  if (!conn) {
    return;
  }

  dbus_connection_read_write(conn, 0);

  DBusMessage *msg = dbus_connection_pop_message(conn);
  if (!msg) {
    return;
  }

  if (!dbus_message_is_signal(msg, "org.freedesktop.DBus.Properties", "PropertiesChanged")) {
    dbus_message_unref(msg);
    return;
  }

  const char *path = dbus_message_get_path(msg);
  if (!path) {
    dbus_message_unref(msg);
    return;
  }

  // Extract "Value" bytes
  std::vector<uint8_t> bytes;
  {
    DBusMessageIter args;
    dbus_message_iter_init(msg, &args);

    const char *iface = nullptr;
    dbus_message_iter_get_basic(&args, &iface);

    if (!iface || std::strcmp(iface, "org.bluez.GattCharacteristic1") != 0) {
      dbus_message_unref(msg);
      return;
    }

    dbus_message_iter_next(&args);
    DBusMessageIter dict;
    dbus_message_iter_recurse(&args, &dict);

    while (dbus_message_iter_get_arg_type(&dict) != DBUS_TYPE_INVALID) {
      DBusMessageIter entry;
      dbus_message_iter_recurse(&dict, &entry);

      const char *key = nullptr;
      dbus_message_iter_get_basic(&entry, &key);

      dbus_message_iter_next(&entry);
      DBusMessageIter variant;
      dbus_message_iter_recurse(&entry, &variant);

      if (key && std::strcmp(key, "Value") == 0) {
        DBusMessageIter array;
        dbus_message_iter_recurse(&variant, &array);

        while (dbus_message_iter_get_arg_type(&array) == DBUS_TYPE_BYTE) {
          uint8_t b;
          dbus_message_iter_get_basic(&array, &b);
          bytes.push_back(b);
          dbus_message_iter_next(&array);
        }
      }

      dbus_message_iter_next(&dict);
    }
  }

  dbus_message_unref(msg);

  // Route to correct InputCtx
  for (auto &kv : aelkey_state.input_map) {
    InputCtx &ctx = kv.second;

    if (ctx.decl.type != "gatt") {
      continue;
    }

    // Match by primary characteristic path
    if (ctx.decl.devnode != path) {
      continue;
    }

    // Build Lua callback table
    if (!ctx.decl.callback_events.empty()) {
      lua_getglobal(L, ctx.decl.callback_events.c_str());
      if (lua_isfunction(L, -1)) {
        lua_newtable(L);

        lua_pushstring(L, ctx.decl.id.c_str());
        lua_setfield(L, -2, "device");

        lua_pushstring(L, path);
        lua_setfield(L, -2, "path");

        lua_pushlstring(L, reinterpret_cast<const char *>(bytes.data()), bytes.size());
        lua_setfield(L, -2, "data");

        lua_pushinteger(L, bytes.size());
        lua_setfield(L, -2, "size");

        lua_pushstring(L, "ok");
        lua_setfield(L, -2, "status");

        if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
          std::string emsg = std::format("Lua gatt_callback error: {}", lua_tostring(L, -1));
          lua_warning(L, emsg.c_str(), 0);
          lua_pop(L, 1);
        }
      } else {
        lua_pop(L, 1);
      }
    }

    break;
  }
}

// ---------------------------------------------------------------------
// Device matching
// ---------------------------------------------------------------------

static DBusMessage *get_managed_objects() {
  DBusMessage *msg = dbus_message_new_method_call(
      "org.bluez", "/", "org.freedesktop.DBus.ObjectManager", "GetManagedObjects"
  );

  DBusMessage *resp =
      dbus_connection_send_with_reply_and_block(aelkey_state.g_dbus_conn, msg, -1, nullptr);

  dbus_message_unref(msg);
  return resp;
}

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

      if (!decl.uniq.empty() && address == decl.uniq) {
        match = true;
      }

      if (!match && !decl.name.empty() && (name == decl.name || alias == decl.name)) {
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

std::string match_gatt_device(const InputDecl &decl) {
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

  if (!decl.service) {
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

  if (!decl.characteristic) {
    dbus_message_unref(resp);
    return services[0];
  }

  // 3. characteristics
  dbus_message_iter_init(resp, &iter);
  dbus_message_iter_recurse(&iter, &dict);

  auto characteristics = get_matching_characteristic(decl, services, dict);

  dbus_message_unref(resp);

  if (characteristics.empty()) {
    std::cerr << "GATT match: no matching characteristic found\n";
    return {};
  }

  return characteristics[0];
}

// ---------------------------------------------------------------------
// Low-level GATT read/write helpers
// ---------------------------------------------------------------------

bool gatt_read_characteristic(const std::string &char_path, std::vector<uint8_t> &out_data) {
  out_data.clear();

  DBusConnection *conn = aelkey_state.g_dbus_conn;
  if (!conn) {
    std::cerr << "GATT read: no D-Bus connection\n";
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
    std::cerr << "GATT read: ReadValue failed\n";
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
  DBusConnection *conn = aelkey_state.g_dbus_conn;
  if (!conn) {
    std::cerr << "GATT write: no D-Bus connection\n";
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
    std::cerr << "GATT write: WriteValue failed\n";
    return false;
  }

  dbus_message_unref(reply);
  return true;
}

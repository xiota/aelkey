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

InputCtx attach_gatt_device(const InputDecl &decl) {
  InputCtx ctx;
  ctx.decl = decl;

  DBusConnection *conn = aelkey_state.g_dbus_conn;
  if (!conn) {
    std::cerr << "GATT: no D-Bus connection\n";
    return ctx;
  }

  if (decl.devnode.empty()) {
    std::cerr << "GATT: no characteristic path in devnode for " << decl.id << std::endl;
    return ctx;
  }

  // Derive device GATT root path from the primary characteristic path.
  ctx.gatt_path = derive_device_path_from_char_path(decl.devnode);
  if (ctx.gatt_path.empty()) {
    std::cerr << "GATT: failed to derive device path from " << decl.devnode << std::endl;
  }

  // Install DBus match rule for this characteristic
  {
    DBusError err;
    dbus_error_init(&err);

    std::string rule =
        "type='signal',interface='org.freedesktop.DBus.Properties',"
        "member='PropertiesChanged',path='" +
        decl.devnode + "'";

    dbus_bus_add_match(conn, rule.c_str(), &err);
    dbus_connection_flush(conn);

    if (dbus_error_is_set(&err)) {
      std::cerr << "GATT: Failed to add match rule: " << err.message << std::endl;
      dbus_error_free(&err);
    } else {
      std::cerr << "GATT: Added match rule for " << decl.devnode << std::endl;
    }
  }

  start_notify(conn, decl.devnode);

  ctx.active = true;
  std::cerr << "Attached GATT characteristic: " << decl.devnode << std::endl;

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
// Device + primary characteristic matching
// ---------------------------------------------------------------------

std::string match_gatt_characteristic(const InputDecl &decl) {
  ensure_gatt_initialized();

  DBusMessage *reply = nullptr;
  DBusMessageIter iter, dict;

  // Query all BlueZ objects
  reply = dbus_message_new_method_call(
      "org.bluez", "/", "org.freedesktop.DBus.ObjectManager", "GetManagedObjects"
  );

  DBusMessage *resp =
      dbus_connection_send_with_reply_and_block(aelkey_state.g_dbus_conn, reply, -1, nullptr);
  dbus_message_unref(reply);

  if (!resp) {
    std::cerr << "GATT match: DBus GetManagedObjects failed\n";
    return {};
  }

  if (!dbus_message_iter_init(resp, &iter)) {
    std::cerr << "GATT match: empty DBus reply\n";
    dbus_message_unref(resp);
    return {};
  }

  dbus_message_iter_recurse(&iter, &dict);

  std::vector<std::string> candidate_devices;

  // ------------------------------------------------------------
  // PASS 1: Collect all matching devices
  // ------------------------------------------------------------
  while (dbus_message_iter_get_arg_type(&dict) != DBUS_TYPE_INVALID) {
    DBusMessageIter entry, iface_dict;
    const char *object_path = nullptr;

    dbus_message_iter_recurse(&dict, &entry);
    dbus_message_iter_get_basic(&entry, &object_path);
    dbus_message_iter_next(&entry);
    dbus_message_iter_recurse(&entry, &iface_dict);

    bool is_device = false;
    std::string dev_name, dev_alias, dev_address;

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
            dev_name = v ? v : "";
          } else if (strcmp(key, "Alias") == 0) {
            const char *v = nullptr;
            dbus_message_iter_get_basic(&var, &v);
            dev_alias = v ? v : "";
          } else if (strcmp(key, "Address") == 0) {
            const char *v = nullptr;
            dbus_message_iter_get_basic(&var, &v);
            dev_address = v ? v : "";
          }

          dbus_message_iter_next(&prop_dict);
        }
      }

      dbus_message_iter_next(&iface_dict);
    }

    if (is_device) {
      bool match = false;

      if (!decl.uniq.empty() && dev_address == decl.uniq) {
        match = true;
      }

      if (!match && !decl.name.empty() && (dev_name == decl.name || dev_alias == decl.name)) {
        match = true;
      }

      if (match) {
        candidate_devices.push_back(object_path);
      }
    }

    dbus_message_iter_next(&dict);
  }

  if (candidate_devices.empty()) {
    std::cerr << "GATT match: no matching device found\n";
    dbus_message_unref(resp);
    return {};
  }

  // ------------------------------------------------------------
  // PASS 2: For each device, collect matching services
  // ------------------------------------------------------------
  std::vector<std::string> candidate_services;

  for (const auto &dev_path : candidate_devices) {
    dbus_message_iter_init(resp, &iter);
    dbus_message_iter_recurse(&iter, &dict);

    while (dbus_message_iter_get_arg_type(&dict) != DBUS_TYPE_INVALID) {
      DBusMessageIter entry, iface_dict;
      const char *object_path = nullptr;

      dbus_message_iter_recurse(&dict, &entry);
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
            if (handle == decl.service) {
              candidate_services.push_back(object_path);
            }
          }
        }

        dbus_message_iter_next(&iface_dict);
      }

      dbus_message_iter_next(&dict);
    }
  }

  if (candidate_services.empty()) {
    std::cerr << "GATT match: no matching service found\n";
    dbus_message_unref(resp);
    return {};
  }

  // ------------------------------------------------------------
  // PASS 3: For each service, find matching characteristic
  // ------------------------------------------------------------
  for (const auto &svc_path : candidate_services) {
    dbus_message_iter_init(resp, &iter);
    dbus_message_iter_recurse(&iter, &dict);

    while (dbus_message_iter_get_arg_type(&dict) != DBUS_TYPE_INVALID) {
      DBusMessageIter entry, iface_dict;
      const char *object_path = nullptr;

      dbus_message_iter_recurse(&dict, &entry);
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
            if (handle == decl.characteristic) {
              std::string result = object_path;
              dbus_message_unref(resp);
              return result;
            }
          }
        }

        dbus_message_iter_next(&iface_dict);
      }

      dbus_message_iter_next(&dict);
    }
  }

  std::cerr << "GATT match: no matching characteristic found\n";
  dbus_message_unref(resp);
  return {};
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

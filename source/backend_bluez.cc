#include "backend_bluez.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <sdbus-c++/sdbus-c++.h>

#include "aelkey_state.h"
#include "manager_device.h"
#include "utils/regex_match.h"

bool BackendBluez::on_init() {
  ensure_client();
  if (conn_) {
    tok_shutdown_ = AelkeyState::instance().subscribe_shutdown([this]() { this->shutdown(); });
    return true;
  }
  return false;
}

bool BackendBluez::ensure_client() {
  if (conn_) {
    return true;
  }

  try {
    conn_ = sdbus::createSystemBusConnection();
    conn_->enterEventLoopAsync();

    std::string rule =
        "type='signal',"
        "sender='org.bluez',"
        "interface='org.freedesktop.DBus.Properties',"
        "member='PropertiesChanged',"
        "arg0='org.bluez.Device1'";

    sdbus::Slot slot = conn_->addMatch(
        rule,
        [this](sdbus::Message msg) { on_device_properties_changed(msg); },
        sdbus::return_slot
    );
    monitor_ = std::move(slot);

    return true;
  } catch (const sdbus::Error &e) {
    std::fprintf(
        stderr,
        "BackendBluez: failed to setup system bus connection (%s: %s)\n",
        e.getName().c_str(),
        e.getMessage().c_str()
    );
    conn_.reset();  // Ensure conn_ remains clean/null on failure
    return false;
  } catch (const std::exception &e) {
    std::fprintf(
        stderr, "BackendBluez: unexpected error setting up connection: %s\n", e.what()
    );
    conn_.reset();
    return false;
  }
}

void BackendBluez::shutdown() {
  monitor_ = sdbus::Slot{};

  if (conn_) {
    for (const auto &dev_path : acquired_devs_) {
      disconnect_device(dev_path);
    }
    acquired_devs_.clear();

    conn_.reset();
  }
}

bool BackendBluez::disconnect_device(const std::string &path) {
  if (!ensure_client()) {
    return false;
  }

  std::string device_path = derive_device_path_from_char_path(path);

  try {
    auto proxy = sdbus::createProxy(
        *conn_, sdbus::ServiceName{ "org.bluez" }, sdbus::ObjectPath{ device_path }
    );

    proxy->callMethod("Disconnect").onInterface("org.bluez.Device1");
    return true;
  } catch (const sdbus::Error &e) {
    std::fprintf(
        stderr,
        "BackendBluez: failed to disconnect device on %s (%s: %s)\n",
        device_path.c_str(),
        e.getName().c_str(),
        e.getMessage().c_str()
    );
    return false;
  } catch (const std::exception &e) {
    std::fprintf(
        stderr,
        "BackendBluez: unexpected error starting notify on %s: %s\n",
        device_path.c_str(),
        e.what()
    );
    return false;
  }
}

void BackendBluez::on_properties_changed(sdbus::Message &msg) {
  try {
    std::string iface;
    std::map<std::string, sdbus::Variant> changed;
    std::vector<std::string> invalidated;

    msg >> iface >> changed >> invalidated;

    if (iface != "org.bluez.GattCharacteristic1") {
      return;
    }

    auto it = changed.find("Value");
    if (it == changed.end()) {
      return;
    }

    std::vector<uint8_t> bytes = it->second.get<std::vector<uint8_t>>();

    std::string path = msg.getPath();
  } catch (const sdbus::Error &e) {
    std::fprintf(
        stderr,
        "BackendBluez: D-Bus error in handle_properties_changed (%s: %s)\n",
        e.getName().c_str(),
        e.getMessage().c_str()
    );
  } catch (const std::exception &e) {
    std::fprintf(
        stderr, "BackendBluez: Exception in handle_properties_changed: %s\n", e.what()
    );
  }
}

void BackendBluez::on_device_properties_changed(sdbus::Message &msg) {
  try {
    std::string iface;
    std::map<std::string, sdbus::Variant> changed;
    std::vector<std::string> invalidated;

    msg >> iface >> changed >> invalidated;

    if (iface != "org.bluez.Device1") {
      return;
    }

    auto it = changed.find("ServicesResolved");
    if (it == changed.end()) {
      return;
    }

    bool resolved = it->second.get<bool>();

    if (resolved) {
      auto &state = AelkeyState::instance();
      for (auto &decl : state.input_decls) {
        if (decl.type == "gatt") {
          // Skip if already attached
          if (!decl.devnode.empty()) {
            continue;
          }

          auto &devmgr = ManagerDevice::instance();
          std::string devnode;
          if (devmgr.match_input(decl, devnode)) {
            if (devmgr.attach_input(devnode, decl)) {
              decl.devnode = devnode;
            }
          }
        }
      }
    }
  } catch (const sdbus::Error &e) {
    std::fprintf(
        stderr,
        "BackendBluez: D-Bus error in handle_properties_changed (%s: %s)\n",
        e.getName().c_str(),
        e.getMessage().c_str()
    );
  } catch (const std::exception &e) {
    std::fprintf(
        stderr, "BackendBluez: Exception in handle_properties_changed: %s\n", e.what()
    );
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
    return char_path;
  }
  return char_path.substr(0, pos);
}

BackendBluez::ManagedObjects BackendBluez::get_managed_objects() {
  if (!ensure_client()) {
    return {};
  }

  try {
    auto proxy =
        sdbus::createProxy(*conn_, sdbus::ServiceName{ "org.bluez" }, sdbus::ObjectPath{ "/" });

    ManagedObjects objs;
    proxy->callMethod("GetManagedObjects")
        .onInterface("org.freedesktop.DBus.ObjectManager")
        .storeResultsTo(objs);

    return objs;
  } catch (const sdbus::Error &e) {
    std::fprintf(
        stderr,
        "BackendBluez: failed to get managed objects (%s: %s)\n",
        e.getName().c_str(),
        e.getMessage().c_str()
    );
    return {};
  } catch (const std::exception &e) {
    std::fprintf(
        stderr, "BackendBluez: unexpected error getting managed objects: %s\n", e.what()
    );
    return {};
  }
}

std::string BackendBluez::get_characteristic_uuid(const std::string &path) {
  if (!ensure_client()) {
    return {};
  }

  try {
    auto proxy = sdbus::createProxy(
        *conn_, sdbus::ServiceName{ "org.bluez" }, sdbus::ObjectPath{ path }
    );

    sdbus::Variant v;
    proxy->callMethod("Get")
        .onInterface("org.freedesktop.DBus.Properties")
        .withArguments(std::string("org.bluez.GattCharacteristic1"), std::string("UUID"))
        .storeResultsTo(v);

    return v.get<std::string>();

  } catch (const sdbus::Error &e) {
    std::fprintf(
        stderr,
        "BackendBluez: failed to get characteristic UUID for %s (%s: %s)\n",
        path.c_str(),
        e.getName().c_str(),
        e.getMessage().c_str()
    );
    return {};
  } catch (const std::exception &e) {
    std::fprintf(
        stderr,
        "BackendBluez: unexpected error getting characteristic UUID for %s: %s\n",
        path.c_str(),
        e.what()
    );
    return {};
  }
}

std::vector<std::string> BackendBluez::get_characteristic_flags(const std::string &path) {
  if (!ensure_client()) {
    return {};
  }

  try {
    auto proxy = sdbus::createProxy(
        *conn_, sdbus::ServiceName{ "org.bluez" }, sdbus::ObjectPath{ path }
    );

    sdbus::Variant v;
    proxy->callMethod("Get")
        .onInterface("org.freedesktop.DBus.Properties")
        .withArguments(std::string("org.bluez.GattCharacteristic1"), std::string("Flags"))
        .storeResultsTo(v);

    return v.get<std::vector<std::string>>();

  } catch (const sdbus::Error &e) {
    std::fprintf(
        stderr,
        "BackendBluez: failed to get characteristic flags for %s (%s: %s)\n",
        path.c_str(),
        e.getName().c_str(),
        e.getMessage().c_str()
    );
    return {};
  } catch (const std::exception &e) {
    std::fprintf(
        stderr,
        "BackendBluez: unexpected error getting characteristic flags for %s: %s\n",
        path.c_str(),
        e.what()
    );
    return {};
  }
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
  const auto flags = get_characteristic_flags(char_path);

  for (const auto &flag : flags) {
    if (flag == "notify" || flag == "indicate") {
      return true;
    }
  }

  return false;
}

std::string BackendBluez::resolve_gatt_paths(
    const InputDecl &decl,
    std::vector<std::string> *found_characteristics
) {
  auto objs = get_managed_objects();

  auto devices = get_matching_devices(decl, objs);
  if (devices.empty()) {
    return {};
  }

  if (decl.services.empty() && !found_characteristics) {
    return devices[0];
  }

  auto services = get_matching_services(decl, devices, objs);
  if (services.empty()) {
    return {};
  }

  if (decl.characteristics.empty() && !found_characteristics) {
    return services[0];
  }

  auto characteristics = get_matching_characteristics(decl, services, objs);

  if (found_characteristics) {
    *found_characteristics = characteristics;
  }

  if (decl.services.empty()) {
    return devices[0];
  }

  if (decl.characteristics.empty()) {
    return services[0];
  }

  if (characteristics.empty()) {
    std::fprintf(
        stderr, "BackendBluez: failed to resolve GATT path: no matching characteristic found\n"
    );
    return {};
  }

  return characteristics[0];
}

std::vector<std::string>
BackendBluez::get_matching_devices(const InputDecl &decl, const ManagedObjects &objs) {
  std::vector<std::string> result;

  for (const auto &[object_path, ifaces] : objs) {
    auto devIt = ifaces.find("org.bluez.Device1");
    if (devIt == ifaces.end()) {
      continue;
    }

    bool is_device = true;
    std::string name, alias, address;

    for (const auto &[key, val] : devIt->second) {
      try {
        if (key == "Name") {
          name = val.get<std::string>();
        } else if (key == "Alias") {
          alias = val.get<std::string>();
        } else if (key == "Address") {
          address = val.get<std::string>();
        }
      } catch (const sdbus::Error &e) {
        std::fprintf(
            stderr,
            "BackendBluez: failed to extract device property '%s' for %s (%s: %s)\n",
            key.c_str(),
            object_path.c_str(),
            e.getName().c_str(),
            e.getMessage().c_str()
        );
      } catch (const std::exception &e) {
        std::fprintf(
            stderr,
            "BackendBluez: unexpected error extracting property '%s' for %s: %s\n",
            key.c_str(),
            object_path.c_str(),
            e.what()
        );
      }
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
  }

  return result;
}

std::vector<std::string> BackendBluez::get_matching_services(
    const InputDecl &decl,
    const std::vector<std::string> &candidate_devices,
    const ManagedObjects &objs
) {
  std::vector<std::string> result;

  for (const auto &dev_path : candidate_devices) {
    for (const auto &[object_path, ifaces] : objs) {
      auto svcIt = ifaces.find("org.bluez.GattService1");
      if (svcIt == ifaces.end()) {
        continue;
      }

      if (object_path.rfind(dev_path, 0) != 0) {
        continue;
      }

      const char *p = std::strstr(object_path.c_str(), "service");
      if (!p) {
        continue;
      }

      int handle = std::strtoul(p + 7, nullptr, 16);
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

  return result;
}

std::vector<std::string> BackendBluez::get_matching_characteristics(
    const InputDecl &decl,
    const std::vector<std::string> &candidate_services,
    const ManagedObjects &objs
) {
  std::vector<std::string> result;

  for (const auto &svc_path : candidate_services) {
    for (const auto &[object_path, ifaces] : objs) {
      auto chrIt = ifaces.find("org.bluez.GattCharacteristic1");
      if (chrIt == ifaces.end()) {
        continue;
      }

      if (object_path.rfind(svc_path, 0) != 0) {
        continue;
      }

      const char *p = std::strstr(object_path.c_str(), "char");
      if (!p) {
        continue;
      }

      int handle = std::strtoul(p + 4, nullptr, 16);
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

  return result;
}

bool BackendBluez::read_characteristic(
    const std::string &char_path,
    std::vector<uint8_t> &out_data
) {
  if (!ensure_client()) {
    return false;
  }

  out_data.clear();

  try {
    auto proxy = sdbus::createProxy(
        *conn_, sdbus::ServiceName{ "org.bluez" }, sdbus::ObjectPath{ char_path }
    );

    std::map<std::string, sdbus::Variant> options;

    proxy->callMethod("ReadValue")
        .onInterface("org.bluez.GattCharacteristic1")
        .withArguments(options)
        .storeResultsTo(out_data);

    return true;
  } catch (const sdbus::Error &e) {
    std::fprintf(
        stderr,
        "BackendBluez: failed to read characteristic %s (%s: %s)\n",
        char_path.c_str(),
        e.getName().c_str(),
        e.getMessage().c_str()
    );
    return false;
  } catch (const std::exception &e) {
    std::fprintf(
        stderr,
        "BackendBluez: unexpected error reading characteristic %s: %s\n",
        char_path.c_str(),
        e.what()
    );
    return false;
  }
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

  try {
    auto proxy = sdbus::createProxy(
        *conn_, sdbus::ServiceName{ "org.bluez" }, sdbus::ObjectPath{ char_path }
    );

    std::vector<uint8_t> bytes(data, data + len);

    std::map<std::string, sdbus::Variant> options;
    if (with_resp) {
      options["type"] = sdbus::Variant{ "request" };
    }

    proxy->callMethod("WriteValue")
        .onInterface("org.bluez.GattCharacteristic1")
        .withArguments(bytes, options);

    return true;
  } catch (const sdbus::Error &e) {
    std::fprintf(
        stderr,
        "BackendBluez: failed to write characteristic %s (%s: %s)\n",
        char_path.c_str(),
        e.getName().c_str(),
        e.getMessage().c_str()
    );
    return false;
  } catch (const std::exception &e) {
    std::fprintf(
        stderr,
        "BackendBluez: unexpected error writing characteristic %s: %s\n",
        char_path.c_str(),
        e.what()
    );
    return false;
  }
}

GattNotifySession BackendBluez::acquire_notify(const std::string &char_path) {
  if (!ensure_client()) {
    return { -1, 0 };
  }

  try {
    auto proxy = sdbus::createProxy(
        *conn_, sdbus::ServiceName{ "org.bluez" }, sdbus::ObjectPath{ char_path }
    );

    std::map<std::string, sdbus::Variant> options;
    sdbus::UnixFd fd;
    uint16_t mtu = 0;

    proxy->callMethod("AcquireNotify")
        .onInterface("org.bluez.GattCharacteristic1")
        .withArguments(options)
        .storeResultsTo(fd, mtu);

    std::string dev_path = derive_device_path_from_char_path(char_path);
    if (!dev_path.empty()) {
      acquired_devs_.insert(dev_path);
    }

    return { fd.release(), mtu };
  } catch (const sdbus::Error &e) {
    std::fprintf(
        stderr,
        "BackendBluez: failed to acquire notify on %s (%s: %s)\n",
        char_path.c_str(),
        e.getName().c_str(),
        e.getMessage().c_str()
    );
    return { -1, 0 };
  }
}

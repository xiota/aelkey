#include "device_output.h"

#include <iostream>
#include <string>
#include <vector>

#include <libevdev/libevdev.h>
#include <sol/sol.hpp>

#include "aelkey_state.h"
#include "device_capabilities.h"

// Provide sensible max ranges for ABS axes
static input_absinfo pos_default = { 0, 0, 65535, 0, 0, 0 };
static input_absinfo stick_default = { 0, -32767, 32767, 0, 0, 0 };
static input_absinfo trigger_default = { 0, 0, 255, 0, 0, 0 };
static input_absinfo pressure_default = { 0, 0, 65535, 0, 0, 0 };
static input_absinfo tilt_default = { 0, -90, 90, 0, 0, 0 };
static input_absinfo distance_default = { 0, 0, 255, 0, 0, 0 };
static input_absinfo orient_default = { 0, 0, 3, 0, 0, 0 };
static input_absinfo wheel_default = { 0, -32768, 32767, 0, 0, 0 };
static input_absinfo hat_default = { 0, -1, 1, 0, 0, 0 };

// multitouch defaults
static input_absinfo mt_pos_default = { 0, 0, 65535, 0, 0, 0 };       // positions
static input_absinfo mt_slot_default = { 0, 0, 4, 0, 0, 0 };          // 5 slots (0–4)
static input_absinfo mt_trackid_default = { 0, -1, 65535, 0, 0, 0 };  // tracking IDs
static input_absinfo mt_tooltype_default = { 0, 0, 2, 0, 0, 0 };      // finger/pen/palm
static input_absinfo mt_misc_default = { 0, 0, 255, 0, 0, 0 };        // pressure/size

static const input_absinfo *default_absinfo_for(int code) {
  switch (code) {
    // Sticks
    case ABS_RX:
    case ABS_RY:
      return &stick_default;

    // Coordinates (Tablets, Digitizers, etc)
    case ABS_X:
    case ABS_Y:
      return &pos_default;

    // Triggers / pedals
    case ABS_Z:
    case ABS_RZ:
    case ABS_THROTTLE:
    case ABS_BRAKE:
    case ABS_GAS:
    case ABS_RUDDER:
      return &trigger_default;

    // Pressure / touch
    case ABS_PRESSURE:
    case ABS_MT_PRESSURE:
      return &pressure_default;

    // Tilt
    case ABS_TILT_X:
    case ABS_TILT_Y:
      return &tilt_default;

    // Distance / orientation
    case ABS_DISTANCE:
      return &distance_default;
    case ABS_MT_ORIENTATION:
      return &orient_default;

    // Wheel / steering
    case ABS_WHEEL:
      return &wheel_default;

    // Hats (d‑pad)
    case ABS_HAT0X:
    case ABS_HAT0Y:
    case ABS_HAT1X:
    case ABS_HAT1Y:
    case ABS_HAT2X:
    case ABS_HAT2Y:
    case ABS_HAT3X:
    case ABS_HAT3Y:
      return &hat_default;

    // Multitouch positions and slots
    case ABS_MT_POSITION_X:
    case ABS_MT_POSITION_Y:
      return &mt_pos_default;
    case ABS_MT_SLOT:
      return &mt_slot_default;
    case ABS_MT_TRACKING_ID:
      return &mt_trackid_default;
    case ABS_MT_TOOL_TYPE:
      return &mt_tooltype_default;
    case ABS_MT_TOUCH_MAJOR:
    case ABS_MT_TOUCH_MINOR:
    case ABS_MT_WIDTH_MAJOR:
    case ABS_MT_WIDTH_MINOR:
      return &mt_misc_default;

    // Miscellaneous
    case ABS_VOLUME:
    case ABS_MISC:
      return &pos_default;

    default:
      return nullptr;
  }
}

template <typename Codes>
static void enable_codes(libevdev *dev, unsigned int type, const Codes &codes) {
  libevdev_enable_event_type(dev, type);
  for (int code : codes) {
    const input_absinfo *absinfo = (type == EV_ABS) ? default_absinfo_for(code) : nullptr;
    libevdev_enable_event_code(dev, type, code, absinfo);
  }
}

void enable_capability(libevdev *dev, const std::string &cap) {
  unsigned int evtype = EV_KEY;

  if (cap.rfind("KEY_", 0) == 0 || cap.rfind("BTN_", 0) == 0) {
    evtype = EV_KEY;
  } else if (cap.rfind("REL_", 0) == 0) {
    evtype = EV_REL;
  } else if (cap.rfind("ABS_", 0) == 0) {
    evtype = EV_ABS;
  } else if (cap.rfind("MSC_", 0) == 0) {
    evtype = EV_MSC;
  } else if (cap.rfind("SW_", 0) == 0) {
    evtype = EV_SW;
  }

  int code = libevdev_event_code_from_name(evtype, cap.c_str());
  if (code >= 0) {
    enable_codes(dev, evtype, std::vector{ code });
  } else {
    std::fprintf(stderr, "Unknown capability string: %s\n", cap.c_str());
  }
}

libevdev_uinput *create_output_device(const OutputDecl &out) {
  struct libevdev *dev = libevdev_new();
  libevdev_set_name(dev, out.name.c_str());
  libevdev_set_id_bustype(dev, out.bus);
  libevdev_set_id_vendor(dev, out.vendor);
  libevdev_set_id_product(dev, out.product);
  libevdev_set_id_version(dev, out.version);

  if (out.type == "keyboard") {
    enable_codes(dev, EV_KEY, aelkey::capabilities::keyboard_keys);

    // scan codes
    libevdev_enable_event_type(dev, EV_MSC);
    libevdev_enable_event_code(dev, EV_MSC, MSC_SCAN, nullptr);

    // repeating event settings
    libevdev_enable_event_type(dev, EV_REP);
    libevdev_enable_event_code(dev, EV_REP, REP_DELAY, nullptr);
    libevdev_enable_event_code(dev, EV_REP, REP_PERIOD, nullptr);
  } else if (out.type == "consumer") {
    enable_codes(dev, EV_KEY, aelkey::capabilities::consumer_keys);
  } else if (out.type == "gamepad") {
    enable_codes(dev, EV_KEY, aelkey::capabilities::gamepad_buttons);
    enable_codes(dev, EV_ABS, aelkey::capabilities::gamepad_abs);

    // Override ABS_X/ABS_Y to stick range
    libevdev_enable_event_code(dev, EV_ABS, ABS_X, &stick_default);
    libevdev_enable_event_code(dev, EV_ABS, ABS_Y, &stick_default);
  } else if (out.type == "mouse") {
    enable_codes(dev, EV_KEY, aelkey::capabilities::mouse_buttons);
    enable_codes(dev, EV_REL, aelkey::capabilities::mouse_rel);
  } else if (out.type == "touchpad") {
    enable_codes(dev, EV_KEY, aelkey::capabilities::touchpad_buttons);
    enable_codes(dev, EV_REL, aelkey::capabilities::touchpad_rel);
    enable_codes(dev, EV_ABS, aelkey::capabilities::touchpad_abs);
    libevdev_enable_property(dev, INPUT_PROP_POINTER);
  } else if (out.type == "touchpad_mt") {
    enable_codes(dev, EV_KEY, aelkey::capabilities::touchpad_buttons);
    enable_codes(dev, EV_ABS, aelkey::capabilities::touchpad_mt_abs);
    libevdev_enable_property(dev, INPUT_PROP_POINTER);
  } else if (out.type == "touchscreen") {
    enable_codes(dev, EV_KEY, aelkey::capabilities::touchscreen_keys);
    enable_codes(dev, EV_ABS, aelkey::capabilities::touchscreen_abs);
    libevdev_enable_property(dev, INPUT_PROP_DIRECT);
  } else if (out.type == "digitizer") {
    enable_codes(dev, EV_KEY, aelkey::capabilities::digitizer_keys);
    enable_codes(dev, EV_ABS, aelkey::capabilities::digitizer_abs);
    libevdev_enable_property(dev, INPUT_PROP_DIRECT);
  }

  for (const auto &cap : out.capabilities) {
    enable_capability(dev, cap);
  }

  struct libevdev_uinput *uidev = nullptr;
  int err = libevdev_uinput_create_from_device(dev, LIBEVDEV_UINPUT_OPEN_MANAGED, &uidev);
  if (err != 0) {
    std::fprintf(stderr, "Failed to create uinput device: %s\n", out.name.c_str());
    libevdev_free(dev);
    return nullptr;
  }

  std::cout << "Created uinput device: " << out.name << " at "
            << libevdev_uinput_get_devnode(uidev) << std::endl;

  libevdev_free(dev);
  return uidev;
}

OutputDecl parse_output(sol::table tbl) {
  OutputDecl decl;

  // id
  if (sol::object v = tbl["id"]; v.valid() && v.is<std::string>()) {
    decl.id = v.as<std::string>();
  }

  // type
  if (sol::object v = tbl["type"]; v.valid() && v.is<std::string>()) {
    decl.type = v.as<std::string>();
  }

  // vendor
  if (sol::object v = tbl["vendor"]; v.valid() && v.is<int>()) {
    decl.vendor = v.as<int>();
  }

  // product
  if (sol::object v = tbl["product"]; v.valid() && v.is<int>()) {
    decl.product = v.as<int>();
  }

  // version
  if (sol::object v = tbl["version"]; v.valid() && v.is<int>()) {
    decl.version = v.as<int>();
  }

  // bus
  if (sol::object v = tbl["bus"]; v.valid() && v.is<std::string>()) {
    std::string busstr = v.as<std::string>();
    if (busstr == "usb") {
      decl.bus = BUS_USB;
    } else if (busstr == "bluetooth") {
      decl.bus = BUS_BLUETOOTH;
    } else if (busstr == "pci") {
      decl.bus = BUS_PCI;
    }
  }

  // name
  if (sol::object v = tbl["name"]; v.valid() && v.is<std::string>()) {
    decl.name = v.as<std::string>();
  }

  // capabilities
  if (sol::object caps_obj = tbl["capabilities"];
      caps_obj.valid() && caps_obj.is<sol::table>()) {
    sol::table caps = caps_obj.as<sol::table>();
    caps.for_each([&](sol::object /*k*/, sol::object v) {
      if (v.is<std::string>()) {
        decl.capabilities.push_back(v.as<std::string>());
      }
    });
  }

  return decl;
}

void parse_outputs_from_lua(sol::this_state ts) {
  sol::state_view lua(ts);

  auto &state = AelkeyState::instance();
  state.output_decls.clear();

  sol::object obj = lua["outputs"];
  if (!obj.valid() || !obj.is<sol::table>()) {
    return;
  }

  sol::table outputs = obj.as<sol::table>();

  outputs.for_each([&](sol::object /*k*/, sol::object v) {
    if (v.is<sol::table>()) {
      OutputDecl decl = parse_output(v.as<sol::table>());
      if (!decl.id.empty()) {
        state.output_decls.push_back(decl);
      }
    }
  });
}

// This helper is still pure C++ and can be used from elsewhere
// once aelkey_state.output_decls has been filled.
void create_outputs_from_decls() {
  auto &state = AelkeyState::instance();
  for (auto &out : state.output_decls) {
    if (out.id.empty()) {
      continue;
    }
    if (state.uinput_devices.count(out.id)) {
      continue;
    }
    libevdev_uinput *uidev = create_output_device(out);
    if (uidev) {
      state.uinput_devices[out.id] = uidev;
    }
  }
}

#include "device_out_uinput.h"

#include <cstdio>
#include <string>
#include <vector>

#include <libevdev/libevdev-uinput.h>
#include <libevdev/libevdev.h>

#include "device_declarations.h"
#include "manager_haptics.h"

// Provide sensible max ranges for ABS axes
// value, min, max, fuzz, flat, resolution
static input_absinfo pos_default = { 0, 0, 4095, 0, 0, 0 };
static input_absinfo stick_default = { 0, -32768, 32767, 16, 128, 0 };
static input_absinfo trigger_default = { 0, 0, 255, 0, 0, 0 };
static input_absinfo pressure_default = { 0, 0, 255, 0, 0, 0 };
static input_absinfo tilt_default = { 0, -90, 90, 0, 0, 0 };
static input_absinfo distance_default = { 0, 0, 255, 0, 0, 0 };
static input_absinfo orient_default = { 0, 0, 3, 0, 0, 0 };
static input_absinfo wheel_default = { 0, -32768, 32767, 0, 0, 0 };
static input_absinfo hat_default = { 0, -1, 1, 0, 0, 0 };

// multitouch defaults
static input_absinfo mt_pos_default = { 0, 0, 4095, 0, 0, 0 };       // positions
static input_absinfo mt_slot_default = { 0, 0, 16, 0, 0, 0 };        // slots
static input_absinfo mt_trackid_default = { 0, 0, 65535, 0, 0, 0 };  // tracking IDs
static input_absinfo mt_tooltype_default = { 0, 0, 2, 0, 0, 0 };     // finger/pen/palm
static input_absinfo mt_misc_default = { 0, 0, 255, 0, 0, 0 };       // pressure/size

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

static void enable_capability(libevdev *dev, const OutputCapability &cap) {
  const std::string &code_str = cap.code;

  if (code_str.rfind("INPUT_PROP_", 0) == 0) {
    int prop = libevdev_property_from_name(code_str.c_str());
    if (prop < 0) {
      std::fprintf(stderr, "Unknown input property string: %s\n", code_str.c_str());
      return;
    }
    libevdev_enable_property(dev, prop);
    return;
  }

  unsigned int evtype = EV_KEY;
  if (code_str.rfind("KEY_", 0) == 0 || code_str.rfind("BTN_", 0) == 0) {
    evtype = EV_KEY;
  } else if (code_str.rfind("REL_", 0) == 0) {
    evtype = EV_REL;
  } else if (code_str.rfind("ABS_", 0) == 0) {
    evtype = EV_ABS;
  } else if (code_str.rfind("MSC_", 0) == 0) {
    evtype = EV_MSC;
  } else if (code_str.rfind("SW_", 0) == 0) {
    evtype = EV_SW;
  } else if (code_str.rfind("FF_", 0) == 0) {
    evtype = EV_FF;
  }

  int code = libevdev_event_code_from_name(evtype, code_str.c_str());
  if (code < 0) {
    std::fprintf(stderr, "Unknown capability string: %s\n", code_str.c_str());
    return;
  }

  libevdev_enable_event_type(dev, evtype);

  if (evtype == EV_ABS) {
    // Start with default absinfo if available, otherwise zero-initialize
    const input_absinfo *def_info = default_absinfo_for(code);
    input_absinfo info = def_info ? *def_info : input_absinfo{ 0, 0, 0, 0, 0, 0 };

    // Override with any explicitly provided custom bounds
    if (cap.min.has_value()) {
      info.minimum = *cap.min;
    }
    if (cap.max.has_value()) {
      info.maximum = *cap.max;
    }
    if (cap.fuzz.has_value()) {
      info.fuzz = *cap.fuzz;
    }
    if (cap.flat.has_value()) {
      info.flat = *cap.flat;
    }
    if (cap.resolution.has_value()) {
      info.resolution = *cap.resolution;
    }

    libevdev_enable_event_code(dev, EV_ABS, code, &info);
  } else {
    libevdev_enable_event_code(dev, evtype, code, nullptr);
  }
}

static libevdev_uinput *create_output_device(const OutputDecl &out) {
  struct libevdev *dev = libevdev_new();
  libevdev_set_name(dev, out.name.c_str());
  libevdev_set_id_bustype(dev, out.bus);
  libevdev_set_id_vendor(dev, out.vendor);
  libevdev_set_id_product(dev, out.product);
  libevdev_set_id_version(dev, out.version);

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

  int ufd = libevdev_uinput_get_fd(uidev);

  ManagerHaptics::instance().register_source(out.id, ufd, out.on_haptics);

  std::printf(
      "Created uinput device: %s at %s\n", out.name.c_str(), libevdev_uinput_get_devnode(uidev)
  );

  libevdev_free(dev);
  return uidev;
}

bool DeviceOutUinput::create(const OutputDecl &decl) {
  libevdev_uinput *uidev = create_output_device(decl);
  if (!uidev) {
    return false;
  }

  // Store device by its declared ID
  devices_[decl.id] = uidev;
  return true;
}

libevdev_uinput *DeviceOutUinput::get(std::string id) const {
  auto it = devices_.find(id);
  return (it != devices_.end()) ? it->second : nullptr;
}

void DeviceOutUinput::send(const std::string &id, int type, int code, int value) {
  if (auto *dev = get(id)) {
    libevdev_uinput_write_event(dev, type, code, value);
  }
}

void DeviceOutUinput::sync(std::string id) {
  if (auto *dev = get(id)) {
    libevdev_uinput_write_event(dev, EV_SYN, SYN_REPORT, 0);
  }
}

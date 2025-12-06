#include "device_output.h"

#include <iostream>

#include <libevdev/libevdev.h>
#include <lua.hpp>

#include "aelkey_state.h"
#include "device_capabilities.h"

// Provide sensible max ranges for ABS axes
// device_output.cc

static const input_absinfo *default_absinfo_for(int code) {
  static input_absinfo pos_default = { 0, 0, 65535, 0, 0, 0 };
  static input_absinfo pressure_default = { 0, 0, 65535, 0, 0, 0 };
  static input_absinfo tilt_default = { 0, -90, 90, 0, 0, 0 };
  static input_absinfo distance_default = { 0, 0, 255, 0, 0, 0 };
  static input_absinfo orient_default = { 0, 0, 3, 0, 0, 0 };
  static input_absinfo wheel_default = { 0, 0, 65535, 0, 0, 0 };

  switch (code) {
    case ABS_X:
    case ABS_Y:
    case ABS_MT_POSITION_X:
    case ABS_MT_POSITION_Y:
      return &pos_default;
    case ABS_PRESSURE:
    case ABS_MT_PRESSURE:
      return &pressure_default;
    case ABS_TILT_X:
    case ABS_TILT_Y:
      return &tilt_default;
    case ABS_DISTANCE:
      return &distance_default;
    case ABS_MT_ORIENTATION:
      return &orient_default;
    case ABS_WHEEL:
      return &wheel_default;
    default:
      return nullptr;
  }
}

static void enable_codes(libevdev *dev, unsigned int type, const auto &codes) {
  libevdev_enable_event_type(dev, type);
  for (int code : codes) {
    const input_absinfo *absinfo = (type == EV_ABS) ? default_absinfo_for(code) : nullptr;
    libevdev_enable_event_code(dev, type, code, absinfo);
  }
}

libevdev_uinput *create_output_device(const OutputDecl &out) {
  struct libevdev *dev = libevdev_new();
  libevdev_set_name(dev, out.name.c_str());

  if (out.type == "keyboard") {
    enable_codes(dev, EV_KEY, aelkey::capabilities::keyboard_keys);
  } else if (out.type == "consumer") {
    enable_codes(dev, EV_KEY, aelkey::capabilities::consumer_keys);
  } else if (out.type == "mouse" || out.type == "touchpad") {
    enable_codes(dev, EV_KEY, aelkey::capabilities::mouse_buttons);
    enable_codes(dev, EV_REL, aelkey::capabilities::mouse_rel);

    if (out.type == "touchpad") {
      enable_codes(dev, EV_ABS, aelkey::capabilities::touchpad_abs);
    }
  } else if (out.type == "gamepad") {
    enable_codes(dev, EV_KEY, aelkey::capabilities::gamepad_buttons);
    enable_codes(dev, EV_ABS, aelkey::capabilities::gamepad_abs);
  } else if (out.type == "touchscreen") {
    enable_codes(dev, EV_KEY, aelkey::capabilities::touchscreen_keys);
    enable_codes(dev, EV_ABS, aelkey::capabilities::touchscreen_abs);

  } else if (out.type == "digitizer") {
    enable_codes(dev, EV_KEY, aelkey::capabilities::digitizer_keys);
    enable_codes(dev, EV_ABS, aelkey::capabilities::digitizer_abs);
  }

  struct libevdev_uinput *uidev = nullptr;
  int err = libevdev_uinput_create_from_device(dev, LIBEVDEV_UINPUT_OPEN_MANAGED, &uidev);
  if (err != 0) {
    std::cerr << "Failed to create uinput device: " << out.name << std::endl;
    libevdev_free(dev);
    return nullptr;
  }

  std::cout << "Created uinput device: " << out.name << " at "
            << libevdev_uinput_get_devnode(uidev) << std::endl;

  libevdev_free(dev);
  return uidev;
}

OutputDecl parse_output(lua_State *L, int index) {
  OutputDecl decl;
  lua_pushnil(L);
  while (lua_next(L, index)) {
    std::string key = lua_tostring(L, -2);
    if (key == "id" && lua_isstring(L, -1)) {
      decl.id = lua_tostring(L, -1);
    } else if (key == "type" && lua_isstring(L, -1)) {
      decl.type = lua_tostring(L, -1);
    } else if (key == "name" && lua_isstring(L, -1)) {
      decl.name = lua_tostring(L, -1);
    }
    lua_pop(L, 1);
  }
  return decl;
}

void parse_outputs_from_lua(lua_State *L) {
  aelkey_state.output_decls.clear();

  lua_getglobal(L, "outputs");
  if (!lua_istable(L, -1)) {
    lua_pop(L, 1);
    return;
  }

  lua_pushnil(L);
  while (lua_next(L, -2) != 0) {
    if (lua_istable(L, -1)) {
      OutputDecl decl = parse_output(L, lua_gettop(L));
      if (!decl.id.empty()) {
        aelkey_state.output_decls.push_back(decl);
      }
    }
    lua_pop(L, 1);  // pop value, keep key
  }
  lua_pop(L, 1);  // pop outputs table
}

void create_outputs_from_decls() {
  for (auto &out : aelkey_state.output_decls) {
    if (out.id.empty()) {
      continue;
    }
    if (aelkey_state.uinput_devices.count(out.id)) {
      continue;
    }
    libevdev_uinput *uidev = create_output_device(out);
    if (uidev) {
      aelkey_state.uinput_devices[out.id] = uidev;
    }
  }
}

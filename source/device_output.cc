#include "device_output.h"

#include <iostream>

#include <libevdev/libevdev.h>
#include <lua.hpp>

#include "aelkey_state.h"
#include "device_capabilities.h"

libevdev_uinput *create_output_device(const OutputDecl &out) {
  struct libevdev *dev = libevdev_new();
  libevdev_set_name(dev, out.name.c_str());

  if (out.type == "keyboard") {
    libevdev_enable_event_type(dev, EV_KEY);
    for (int code : aelkey::capabilities::keyboard_keys) {
      libevdev_enable_event_code(dev, EV_KEY, code, nullptr);
    }
  } else if (out.type == "consumer") {
    libevdev_enable_event_type(dev, EV_KEY);
    for (int code : aelkey::capabilities::consumer_keys) {
      libevdev_enable_event_code(dev, EV_KEY, code, nullptr);
    }
  } else if (out.type == "mouse") {
    // Buttons
    libevdev_enable_event_type(dev, EV_KEY);

    for (int code : aelkey::capabilities::mouse_buttons) {
      libevdev_enable_event_code(dev, EV_KEY, code, nullptr);
    }

    // Relative axes
    libevdev_enable_event_type(dev, EV_REL);
    for (int code : aelkey::capabilities::mouse_rel) {
      libevdev_enable_event_code(dev, EV_REL, code, nullptr);
    }
  } else if (out.type == "gamepad") {
    libevdev_enable_event_type(dev, EV_KEY);
    for (int code : aelkey::capabilities::gamepad_buttons) {
      libevdev_enable_event_code(dev, EV_KEY, code, nullptr);
    }

    libevdev_enable_event_type(dev, EV_ABS);

    struct input_absinfo abs_default = { 0, -32768, 32767, 0, 0, 0 };
    struct input_absinfo hat_default = { 0, -1, 1, 0, 0, 0 };

    for (int code : aelkey::capabilities::gamepad_abs) {
      if (code == ABS_HAT0X || code == ABS_HAT0Y) {
        libevdev_enable_event_code(dev, EV_ABS, code, &hat_default);
      } else {
        libevdev_enable_event_code(dev, EV_ABS, code, &abs_default);
      }
    }
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

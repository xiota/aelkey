#include "aelkey_device.h"

#include <lua.hpp>
#include <sys/epoll.h>
#include <unistd.h>

#include "aelkey_state.h"
#include "device_input.h"
#include "device_udev.h"

static void create_outputs_from_decls() {
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

static void attach_inputs_from_decls(lua_State *L) {
  for (auto &decl : aelkey_state.input_decls) {
    std::string devnode = match_device(decl);
    if (devnode.empty()) {
      continue;
    }
    if (attach_input_device(devnode, decl)) {
      notify_state_change(L, decl, "connect");
    }
  }
}

int lua_open_device(lua_State *L) {
  // If devices already attached, skip
  if (!aelkey_state.input_map.empty() || !aelkey_state.uinput_devices.empty()) {
    lua_pushboolean(L, 1);
    return 1;
  }

  // Ensure init is done (epoll + udev monitor)
  if (aelkey_state.epfd < 0 || aelkey_state.udev_fd < 0 || !aelkey_state.g_udev ||
      !aelkey_state.g_mon) {
    int rc = device_udev_init(L);
    if (rc != 0) {
      // lua_init already pushed an error or returned an error code
      return rc;
    }
  }

  // Parse declarations from Lua and perform initial setup
  // These helpers should read from the script's tables and fill:
  //   aelkey_state.output_decls and aelkey_state.input_decls
  parse_outputs_from_lua(L);
  parse_inputs_from_lua(L);

  create_outputs_from_decls();
  attach_inputs_from_decls(L);

  lua_pushboolean(L, 1);
  return 1;
}

int lua_close_device(lua_State *L) {
  const char *dev_id_cstr = luaL_checkstring(L, 1);
  std::string dev_id(dev_id_cstr);

  auto it = aelkey_state.input_map.find(dev_id);
  if (it == aelkey_state.input_map.end()) {
    lua_pushboolean(L, 0);
    return 1;
  }
  InputCtx &ctx = it->second;

  // If evdev/hidraw, free libevdev resources
  if (ctx.idev) {
    libevdev_grab(ctx.idev, LIBEVDEV_UNGRAB);
    libevdev_free(ctx.idev);
    ctx.idev = nullptr;
  }

  // Remove from epoll if fd is valid
  if (aelkey_state.epfd >= 0 && ctx.fd >= 0) {
    epoll_ctl(aelkey_state.epfd, EPOLL_CTL_DEL, ctx.fd, nullptr);
  }

  // Cleanup maps
  aelkey_state.input_map.erase(dev_id);
  aelkey_state.frames.erase(dev_id);

  // Close the file descriptor if valid
  if (ctx.fd >= 0) {
    close(ctx.fd);
    ctx.fd = -1;
  }

  // Close libusb handle if present
  if (ctx.usb_handle) {
    libusb_close(ctx.usb_handle);
    ctx.usb_handle = nullptr;
  }

  lua_pushboolean(L, 1);
  return 1;
}

int lua_get_device_info(lua_State *L) {
  const char *dev_id_cstr = luaL_checkstring(L, 1);
  std::string dev_id(dev_id_cstr);

  auto it = aelkey_state.input_map.find(dev_id);
  if (it == aelkey_state.input_map.end()) {
    lua_pushnil(L);
    return 1;
  }

  const InputDecl &decl = it->second.decl;

  lua_newtable(L);
  lua_pushstring(L, decl.id.c_str());
  lua_setfield(L, -2, "id");

  lua_pushstring(L, decl.type.c_str());
  lua_setfield(L, -2, "type");

  lua_pushinteger(L, decl.vendor);
  lua_setfield(L, -2, "vendor");

  lua_pushinteger(L, decl.product);
  lua_setfield(L, -2, "product");

  lua_pushinteger(L, decl.bus);
  lua_setfield(L, -2, "bus");

  lua_pushstring(L, decl.name.c_str());
  lua_setfield(L, -2, "name");

  lua_pushstring(L, decl.phys.c_str());
  lua_setfield(L, -2, "phys");

  lua_pushstring(L, decl.uniq.c_str());
  lua_setfield(L, -2, "uniq");

  lua_pushboolean(L, decl.writable);
  lua_setfield(L, -2, "writable");

  lua_pushboolean(L, decl.grab);
  lua_setfield(L, -2, "grab");

  return 1;
}

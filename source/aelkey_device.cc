#include "aelkey_device.h"

#include <lua.hpp>
#include <sys/epoll.h>
#include <unistd.h>

#include "aelkey_state.h"
#include "device_input.h"

int lua_open_device(lua_State *L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  InputDecl decl = parse_input(L, 1);

  if (decl.id.empty()) {
    return luaL_error(L, "open_device: missing 'id' field");
  }

  // Ensure epoll fd exists
  if (aelkey_state.epfd < 0) {
    aelkey_state.epfd = epoll_create1(0);
    if (aelkey_state.epfd < 0) {
      return luaL_error(L, "open_device: failed to create epoll fd");
    }
  }

  // Match device node
  std::string devnode = match_device(decl);
  if (devnode.empty()) {
    lua_pushnil(L);
    return 1;
  }

  // Attach device returns a full InputCtx
  InputCtx ctx = attach_device(
      devnode, decl, aelkey_state.input_map, aelkey_state.frames, aelkey_state.epfd
  );

  // Check for failure: both fd and usb_handle invalid
  if (ctx.fd < 0 && !ctx.usb_handle) {
    lua_pushnil(L);
    return 1;
  }

  // Store context keyed by string id
  aelkey_state.input_map[decl.id] = std::move(ctx);

  // Return the string id to Lua, not the fd
  lua_pushstring(L, decl.id.c_str());
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

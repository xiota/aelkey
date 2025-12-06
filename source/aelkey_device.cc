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

  if (aelkey_state.epfd < 0) {
    aelkey_state.epfd = epoll_create1(0);
    if (aelkey_state.epfd < 0) {
      return luaL_error(L, "open_device: failed to create epoll fd");
    }
  }

  std::string devnode = match_device(decl);
  if (devnode.empty()) {
    lua_pushnil(L);
    return 1;
  }

  int fd = attach_device(
      devnode, decl, aelkey_state.input_map, aelkey_state.frames, aelkey_state.epfd
  );
  if (fd < 0) {
    lua_pushnil(L);
    return 1;
  }

  aelkey_state.devnode_to_fd[decl.id] = fd;
  lua_pushinteger(L, fd);
  return 1;
}

int lua_close_device(lua_State *L) {
  int dev_id = luaL_checkinteger(L, 1);

  auto it = aelkey_state.input_map.find(dev_id);
  if (it == aelkey_state.input_map.end()) {
    lua_pushboolean(L, 0);
    return 1;
  }

  InputCtx &ctx = it->second;

  // If evdev, free libevdev resources
  if (ctx.idev) {
    libevdev_grab(ctx.idev, LIBEVDEV_UNGRAB);
    libevdev_free(ctx.idev);
  }

  // Remove from epoll if epfd is valid
  if (aelkey_state.epfd >= 0) {
    epoll_ctl(aelkey_state.epfd, EPOLL_CTL_DEL, dev_id, nullptr);
  }

  // Cleanup maps
  aelkey_state.input_map.erase(it);
  aelkey_state.frames.erase(dev_id);
  aelkey_state.devnode_to_fd.erase(ctx.decl.id);

  // Close the file descriptor
  close(dev_id);

  lua_pushboolean(L, 1);
  return 1;
}

int lua_get_device_info(lua_State *L) {
  int dev_id = luaL_checkinteger(L, 1);

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

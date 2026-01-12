#include "aelkey_haptics.h"

#include <cstdio>

#include <sol/sol.hpp>
#include <sys/ioctl.h>
#include <unistd.h>

#include "aelkey_state.h"

// Register a virtual FF source (uinput device)
void haptics_register_source(const std::string &id, int uinput_fd) {
  auto &state = AelkeyState::instance();

  HapticsSourceCtx ctx;
  ctx.id = id;
  ctx.fd = uinput_fd;

  // Add FD to epoll
  struct epoll_event ev{};
  ev.events = EPOLLIN;
  ev.data.fd = uinput_fd;

  if (epoll_ctl(state.epfd, EPOLL_CTL_ADD, uinput_fd, &ev) < 0) {
    perror("epoll_ctl add haptics source");
  }

  state.haptics_sources[id] = std::move(ctx);

  std::printf("Haptics: registered source '%s' (fd=%d)\n", id.c_str(), uinput_fd);
}

// Handle UI_FF_UPLOAD
bool haptics_handle_upload(HapticsSourceCtx &hctx, int request_id) {
  int fd = hctx.fd;

  struct uinput_ff_upload up{};
  up.request_id = request_id;

  if (ioctl(fd, UI_BEGIN_FF_UPLOAD, &up) < 0) {
    perror("UI_BEGIN_FF_UPLOAD");
    return false;
  }

  int virt_id = up.effect.id;

  // Store the effect
  hctx.effects[virt_id] = up.effect;

  up.retval = 0;

  if (ioctl(fd, UI_END_FF_UPLOAD, &up) < 0) {
    perror("UI_END_FF_UPLOAD");
    return false;
  }

  std::printf("Haptics: stored effect virt_id=%d on source '%s'\n", virt_id, hctx.id.c_str());

  return true;
}

// Handle UI_FF_ERASE
bool haptics_handle_erase(HapticsSourceCtx &hctx, int request_id) {
  int fd = hctx.fd;

  struct uinput_ff_erase er{};
  er.request_id = request_id;

  if (ioctl(fd, UI_BEGIN_FF_ERASE, &er) < 0) {
    perror("UI_BEGIN_FF_ERASE");
    return false;
  }

  int virt_id = er.effect_id;

  // Remove effect
  hctx.effects.erase(virt_id);

  er.retval = 0;

  if (ioctl(fd, UI_END_FF_ERASE, &er) < 0) {
    perror("UI_END_FF_ERASE");
    return false;
  }

  std::printf("Haptics: erased effect virt_id=%d on source '%s'\n", virt_id, hctx.id.c_str());

  return true;
}

// debugging helper
void haptics_debug_dump(const HapticsSourceCtx &hctx) {
  std::printf("Haptics: effects for source '%s':\n", hctx.id.c_str());
  for (auto &kv : hctx.effects) {
    const ff_effect &eff = kv.second;
    std::printf("  virt_id=%d type=%d length=%d\n", kv.first, eff.type, eff.replay.length);
  }
}

sol::table haptics_effect_to_lua(sol::state_view lua, const ff_effect &eff) {
  sol::table t = lua.create_table();

  t["id"] = eff.id;
  t["type"] = eff.type;
  t["length"] = eff.replay.length;
  t["delay"] = eff.replay.delay;

  switch (eff.type) {
    case FF_RUMBLE:
      t["strong"] = eff.u.rumble.strong_magnitude;
      t["weak"] = eff.u.rumble.weak_magnitude;
      break;

    case FF_PERIODIC:
      t["waveform"] = eff.u.periodic.waveform;
      t["magnitude"] = eff.u.periodic.magnitude;
      t["offset"] = eff.u.periodic.offset;
      t["phase"] = eff.u.periodic.phase;
      t["period"] = eff.u.periodic.period;
      break;

    case FF_CONSTANT:
      t["level"] = eff.u.constant.level;
      break;

    default:
      break;
  }

  return t;
}

void haptics_handle_play(
    sol::this_state ts,
    HapticsSourceCtx &src,
    int virt_id,
    int magnitude
) {
  sol::state_view lua(ts);

  if (src.callback.empty()) {
    return;
  }

  sol::object cb = lua[src.callback];
  if (!cb.is<sol::function>()) {
    return;
  }

  sol::function f = cb.as<sol::function>();

  sol::table ev = lua.create_table();
  ev["source"] = src.id;
  ev["event"] = "play";
  ev["id"] = virt_id;
  ev["value"] = magnitude;

  auto it = src.effects.find(virt_id);
  if (it != src.effects.end()) {
    ev["effect"] = haptics_effect_to_lua(lua, it->second);
  }

  sol::protected_function pf = f;
  sol::protected_function_result res = pf(ev);
  if (!res.valid()) {
    sol::error err = res;
    std::fprintf(stderr, "Lua haptics callback error: %s\n", err.what());
  }
}

void haptics_handle_stop(sol::this_state ts, HapticsSourceCtx &src, int virt_id) {
  sol::state_view lua(ts);

  if (src.callback.empty()) {
    return;
  }

  sol::object cb = lua[src.callback];
  if (!cb.is<sol::function>()) {
    return;
  }

  sol::function f = cb.as<sol::function>();

  sol::table ev = lua.create_table();
  ev["source"] = src.id;
  ev["event"] = "stop";
  ev["id"] = virt_id;

  sol::protected_function pf = f;
  sol::protected_function_result res = pf(ev);
  if (!res.valid()) {
    sol::error err = res;
    std::fprintf(stderr, "Lua haptics callback error: %s\n", err.what());
  }
}

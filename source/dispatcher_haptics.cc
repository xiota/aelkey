#include "dispatcher_haptics.h"

#include <cerrno>
#include <cstdio>

#include <sys/ioctl.h>
#include <unistd.h>

#include "aelkey_state.h"
#include "haptics_context.h"

DispatcherHaptics::~DispatcherHaptics() {
  cleanup_sources();
}

void DispatcherHaptics::cleanup_sources() {
  for (auto &[id, src] : sources_) {
    if (src.fd >= 0) {
      unregister_fd(src.fd);
      src.fd = -1;
    }
  }
  sources_.clear();
}

void DispatcherHaptics::register_source(
    const std::string &id,
    int uinput_fd,
    const std::string &callback
) {
  HapticsSourceCtx ctx;
  ctx.id = id;
  ctx.fd = uinput_fd;
  ctx.callback = callback;

  register_fd(uinput_fd, EPOLLIN);
  sources_[id] = std::move(ctx);
}

void DispatcherHaptics::propagate_erase_to_sinks(const std::string &source_id, int virt_id) {
  auto &state = AelkeyState::instance();
  auto key = std::make_pair(source_id, virt_id);

  for (auto &[id, ictx] : state.input_map) {
    if (!ictx.haptics.supported) {
      continue;
    }

    auto &sink = ictx.haptics;
    auto it = sink.slots.find(key);
    if (it == sink.slots.end()) {
      continue;
    }

    int real_id = it->second;
    if (ioctl(ictx.fd, EVIOCRMFF, real_id) < 0) {
      perror("EVIOCRMFF");
    }

    sink.slots.erase(it);
  }
}

int DispatcherHaptics::upload_effect_to_sink(
    InputCtx &sink,
    HapticsSinkCtx &hctx,
    ff_effect &eff
) {
  eff.id = -1;

  int rc = ioctl(sink.fd, EVIOCSFF, &eff);

  if (rc < 0 && errno == ENOSPC) {
    for (const auto &[key_pair, r_id] : hctx.slots) {
      ioctl(sink.fd, EVIOCRMFF, r_id);
    }
    hctx.slots.clear();

    rc = ioctl(sink.fd, EVIOCSFF, &eff);
  }

  if (rc < 0) {
    perror("EVIOCSFF");
    return -1;
  }

  return eff.id;
}

ff_effect DispatcherHaptics::lua_to_ff_effect(sol::table t) {
  ff_effect eff{};
  eff.id = -1;

  eff.direction = t.get_or("direction", 0);
  eff.replay.length = t.get_or("length", 250);
  eff.replay.delay = t.get_or("delay", 0);
  eff.trigger.button = t.get_or("trigger_button", 0);
  eff.trigger.interval = t.get_or("trigger_interval", 0);

  std::string type = t.get_or("type", std::string("rumble"));
  if (type == "rumble") {
    eff.type = FF_RUMBLE;
    eff.u.rumble.strong_magnitude = t.get_or("strong", 0x4000);
    eff.u.rumble.weak_magnitude = t.get_or("weak", 0x4000);
  } else if (type == "periodic") {
    eff.type = FF_PERIODIC;
    eff.u.periodic.magnitude = t.get_or("magnitude", 0);
    eff.u.periodic.offset = t.get_or("offset", 0);
    eff.u.periodic.period = t.get_or("period", 0);
    eff.u.periodic.phase = t.get_or("phase", 0);
    eff.u.periodic.waveform = t.get_or("waveform", 0);

    eff.u.periodic.envelope.attack_length = t.get_or("attack_length", 0);
    eff.u.periodic.envelope.attack_level = t.get_or("attack_level", 0);
    eff.u.periodic.envelope.fade_length = t.get_or("fade_length", 0);
    eff.u.periodic.envelope.fade_level = t.get_or("fade_level", 0);
  } else if (type == "constant") {
    eff.type = FF_CONSTANT;
    eff.u.constant.level = t.get_or("level", 0);

    eff.u.constant.envelope.attack_length = t.get_or("attack_length", 0);
    eff.u.constant.envelope.attack_level = t.get_or("attack_level", 0);
    eff.u.constant.envelope.fade_length = t.get_or("fade_length", 0);
    eff.u.constant.envelope.fade_level = t.get_or("fade_level", 0);
  } else {
    eff.type = FF_RUMBLE;
    eff.u.rumble.strong_magnitude = 0x4000;
    eff.u.rumble.weak_magnitude = 0x4000;
    if (!eff.replay.delay) {
      eff.replay.delay = 250;
    }
  }

  return eff;
}

bool DispatcherHaptics::rebuild_effect(const ff_effect &src_eff, ff_effect &out_eff) {
  ff_effect eff{};
  eff.id = -1;

  eff.type = src_eff.type;
  eff.direction = src_eff.direction;
  eff.replay = src_eff.replay;
  eff.trigger = src_eff.trigger;

  switch (src_eff.type) {
    case FF_RUMBLE:
      eff.u.rumble = src_eff.u.rumble;
      break;

    case FF_PERIODIC:
      eff.u.periodic = src_eff.u.periodic;
      eff.u.periodic.envelope = src_eff.u.periodic.envelope;
      break;

    case FF_CONSTANT:
      eff.u.constant = src_eff.u.constant;
      eff.u.constant.envelope = src_eff.u.constant.envelope;
      break;

    default:
      eff.type = FF_RUMBLE;
      eff.u.rumble.strong_magnitude = 0x4000;
      eff.u.rumble.weak_magnitude = 0x4000;
      eff.replay.length = src_eff.replay.length ? src_eff.replay.length : 250;
      break;
  }

  out_eff = eff;
  return true;
}

bool DispatcherHaptics::handle_upload(HapticsSourceCtx &hctx, int request_id) {
  int fd = hctx.fd;

  struct uinput_ff_upload up{};
  up.request_id = request_id;

  if (ioctl(fd, UI_BEGIN_FF_UPLOAD, &up) < 0) {
    perror("UI_BEGIN_FF_UPLOAD");
    return false;
  }

  up.retval = 0;

  if (ioctl(fd, UI_END_FF_UPLOAD, &up) < 0) {
    perror("UI_END_FF_UPLOAD");
    return false;
  }

  int virt_id = up.effect.id;

  ff_effect normalized{};
  if (!rebuild_effect(up.effect, normalized)) {
    std::fprintf(stderr, "Haptics: failed to rebuild effect %d\n", virt_id);
    return false;
  }

  hctx.effects[virt_id] = normalized;

  propagate_erase_to_sinks(hctx.id, virt_id);
  return true;
}

bool DispatcherHaptics::handle_erase(HapticsSourceCtx &hctx, int request_id) {
  int fd = hctx.fd;

  struct uinput_ff_erase er{};
  er.request_id = request_id;

  if (ioctl(fd, UI_BEGIN_FF_ERASE, &er) < 0) {
    perror("UI_BEGIN_FF_ERASE");
    return false;
  }

  int virt_id = er.effect_id;

  hctx.effects.erase(virt_id);
  propagate_erase_to_sinks(hctx.id, virt_id);

  er.retval = 0;

  if (ioctl(fd, UI_END_FF_ERASE, &er) < 0) {
    perror("UI_END_FF_ERASE");
    return false;
  }

  return true;
}

sol::table DispatcherHaptics::haptics_effect_to_lua(sol::state_view lua, const ff_effect &eff) {
  sol::table t = lua.create_table();

  t["id"] = eff.id;
  t["length"] = eff.replay.length;
  t["delay"] = eff.replay.delay;

  switch (eff.type) {
    case FF_RUMBLE:
      t["type"] = "rumble";
      t["strong"] = eff.u.rumble.strong_magnitude;
      t["weak"] = eff.u.rumble.weak_magnitude;
      break;

    case FF_PERIODIC:
      t["type"] = "periodic";
      t["waveform"] = eff.u.periodic.waveform;
      t["magnitude"] = eff.u.periodic.magnitude;
      t["offset"] = eff.u.periodic.offset;
      t["phase"] = eff.u.periodic.phase;
      t["period"] = eff.u.periodic.period;
      break;

    case FF_CONSTANT:
      t["type"] = "constant";
      t["level"] = eff.u.constant.level;
      break;

    default:
      break;
  }

  return t;
}

void DispatcherHaptics::handle_play(
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
  ev["type"] = "play";
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

void DispatcherHaptics::handle_stop(sol::this_state ts, HapticsSourceCtx &src, int virt_id) {
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
  ev["type"] = "stop";
  ev["id"] = virt_id;

  sol::protected_function pf = f;
  sol::protected_function_result res = pf(ev);
  if (!res.valid()) {
    sol::error err = res;
    std::fprintf(stderr, "Lua haptics callback error: %s\n", err.what());
  }
}

void DispatcherHaptics::handle_event(EpollPayload *payload, uint32_t events) {
  if (!(events & EPOLLIN)) {
    return;
  }

  int fd = payload->fd;

  HapticsSourceCtx *src = nullptr;
  for (auto &[id, ctx] : sources_) {
    if (ctx.fd == fd) {
      src = &ctx;
      break;
    }
  }
  if (!src) {
    return;
  }

  struct input_event ev{};
  ssize_t n = ::read(fd, &ev, sizeof(ev));
  if (n < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return;
    }
    perror("read haptics");
    return;
  } else if (n == 0 || n != sizeof(ev)) {
    return;
  }

  sol::state_view lua(AelkeyState::instance().lua_vm);
  sol::this_state ts(lua.lua_state());

  if (ev.type == EV_UINPUT) {
    if (ev.code == UI_FF_UPLOAD) {
      handle_upload(*src, ev.value);
    } else if (ev.code == UI_FF_ERASE) {
      handle_erase(*src, ev.value);
    }
  } else if (ev.type == EV_FF) {
    int virt_id = ev.code;
    int magnitude = ev.value;

    if (magnitude > 0) {
      handle_play(ts, *src, virt_id, magnitude);
    } else {
      handle_stop(ts, *src, virt_id);
    }
  }
}

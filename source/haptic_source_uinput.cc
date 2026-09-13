#include "haptic_source_uinput.h"

#include <cerrno>
#include <cstdio>
#include <linux/uinput.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "aelkey_state.h"
#include "dispatcher_vulgate.h"
#include "manager_haptics.h"

HapticSourceUinput::HapticSourceUinput(std::string id, int fd, std::string callback)
    : id_(std::move(id)), fd_(fd), callback_(std::move(callback)) {
  if (fd_ >= 0) {
    DispatcherCb cb;
    cb.native = [this]() {
      sol::state_view lua(AelkeyState::instance().lua_vm);
      sol::this_state ts(lua.lua_state());
      handle_source_event(ts, ManagerHaptics::instance());
    };
    DispatcherVulgate::instance().register_device_fd(fd_, EPOLLIN, std::move(cb), id_);
  }
}

HapticSourceUinput::~HapticSourceUinput() {
  if (fd_ >= 0) {
    DispatcherVulgate::instance().unregister_device_fd(fd_);
  }
}

bool HapticSourceUinput::rebuild_effect(const ff_effect &src_eff, ff_effect &out_eff) {
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

bool HapticSourceUinput::handle_upload(ManagerHaptics &dispatcher, int request_id) {
  struct uinput_ff_upload up{};
  up.request_id = request_id;

  if (ioctl(fd_, UI_BEGIN_FF_UPLOAD, &up) < 0) {
    perror("UI_BEGIN_FF_UPLOAD");
    return false;
  }

  up.retval = 0;

  if (ioctl(fd_, UI_END_FF_UPLOAD, &up) < 0) {
    perror("UI_END_FF_UPLOAD");
    return false;
  }

  int virt_id = up.effect.id;

  ff_effect normalized{};
  if (!rebuild_effect(up.effect, normalized)) {
    std::fprintf(stderr, "Haptics: failed to rebuild effect %d\n", virt_id);
    return false;
  }

  effects_[virt_id] = normalized;
  dispatcher.propagate_update_to_sinks(id_, virt_id, normalized);

  return true;
}

bool HapticSourceUinput::handle_erase(ManagerHaptics &dispatcher, int request_id) {
  struct uinput_ff_erase er{};
  er.request_id = request_id;

  if (ioctl(fd_, UI_BEGIN_FF_ERASE, &er) < 0) {
    perror("UI_BEGIN_FF_ERASE");
    return false;
  }

  int virt_id = er.effect_id;

  effects_.erase(virt_id);
  dispatcher.propagate_erase_to_sinks(id_, virt_id);

  er.retval = 0;

  if (ioctl(fd_, UI_END_FF_ERASE, &er) < 0) {
    perror("UI_END_FF_ERASE");
    return false;
  }

  return true;
}

sol::table
HapticSourceUinput::haptics_effect_to_lua(sol::state_view lua, const ff_effect &eff) {
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

void HapticSourceUinput::handle_play(sol::this_state ts, int virt_id, int magnitude) {
  sol::state_view lua(ts);

  if (callback_.empty()) {
    return;
  }

  sol::object cb = lua[callback_];
  if (!cb.is<sol::function>()) {
    return;
  }

  sol::function f = cb.as<sol::function>();

  sol::table ev = lua.create_table();
  ev["source"] = id_;
  ev["type"] = "play";
  ev["id"] = virt_id;
  ev["value"] = magnitude;

  auto it = effects_.find(virt_id);
  if (it != effects_.end()) {
    ev["effect"] = haptics_effect_to_lua(lua, it->second);
  }

  sol::protected_function pf = f;
  sol::protected_function_result res = pf(ev);
  if (!res.valid()) {
    sol::error err = res;
    std::fprintf(stderr, "Lua haptics callback error: %s\n", err.what());
  }
}

void HapticSourceUinput::handle_stop(sol::this_state ts, int virt_id) {
  sol::state_view lua(ts);

  if (callback_.empty()) {
    return;
  }

  sol::object cb = lua[callback_];
  if (!cb.is<sol::function>()) {
    return;
  }

  sol::function f = cb.as<sol::function>();

  sol::table ev = lua.create_table();
  ev["source"] = id_;
  ev["type"] = "stop";
  ev["id"] = virt_id;

  sol::protected_function pf = f;
  sol::protected_function_result res = pf(ev);
  if (!res.valid()) {
    sol::error err = res;
    std::fprintf(stderr, "Lua haptics callback error: %s\n", err.what());
  }
}

void HapticSourceUinput::handle_source_event(sol::this_state ts, ManagerHaptics &dispatcher) {
  struct input_event ev{};
  ssize_t n = read(fd_, &ev, sizeof(ev));
  if (n < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return;
    }
    perror("read haptics");
    return;
  } else if (n == 0 || n != sizeof(ev)) {
    return;
  }

  if (ev.type == EV_UINPUT) {
    if (ev.code == UI_FF_UPLOAD) {
      handle_upload(dispatcher, ev.value);
    } else if (ev.code == UI_FF_ERASE) {
      handle_erase(dispatcher, ev.value);
    }
  } else if (ev.type == EV_FF) {
    int virt_id = ev.code;
    int magnitude = ev.value;

    if (magnitude > 0) {
      handle_play(ts, virt_id, magnitude);
    } else {
      handle_stop(ts, virt_id);
    }
  }
}

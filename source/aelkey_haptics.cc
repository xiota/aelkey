#include "aelkey_haptics.h"

#include <cstdio>

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

#include "device_libusb.h"

#include <libusb-1.0/libusb.h>
#include <poll.h>
#include <sys/epoll.h>

#include "aelkey_state.h"

void ensure_libusb_initialized() {
  auto &state = AelkeyState::instance();
  if (!state.g_libusb) {
    if (libusb_init(&state.g_libusb) != 0) {
      throw std::runtime_error("Failed to init libusb");
    }

    // Register pollfd notifiers so epoll stays in sync
    libusb_set_pollfd_notifiers(
        state.g_libusb,
        [](int fd, short events, void *user_data) {
          auto *state = static_cast<AelkeyState *>(user_data);
          epoll_event ev{};
          ev.events = 0;
          if (events & POLLIN) {
            ev.events |= EPOLLIN;
          }
          if (events & POLLOUT) {
            ev.events |= EPOLLOUT;
          }
          ev.data.fd = fd;
          if (epoll_ctl(state->epfd, EPOLL_CTL_ADD, fd, &ev) == 0) {
            state->libusb_fd_set.insert(fd);
          }
        },
        [](int fd, void *user_data) {
          auto *state = static_cast<AelkeyState *>(user_data);
          epoll_ctl(state->epfd, EPOLL_CTL_DEL, fd, nullptr);
          state->libusb_fd_set.erase(fd);
        },
        &state
    );
  }
}

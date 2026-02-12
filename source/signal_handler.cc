#include "signal_handler.h"

#include <csignal>

#include <unistd.h>

#include "aelkey_state.h"
#include "lua_bindings/loop.h"

namespace SignalHandler {

int g_last_signal = 0;

void handle(int sig) {
  auto &state = AelkeyState::instance();
  if (state.loop_running) {
    g_last_signal = sig;
    state.loop_should_stop = true;
    return;
  }

  loop_cleanup();
  _exit(128 + sig);
}

void install() {
  std::signal(SIGHUP, handle);   // terminal hangup
  std::signal(SIGINT, handle);   // interactive interrupt (Ctrl+C)
  std::signal(SIGTERM, handle);  // termination request (kill, systemd stop)
  std::signal(SIGQUIT, handle);  // quit signal (Ctrl-\)
}

void reraise() {
  if (g_last_signal != 0) {
    int sig = g_last_signal;
    g_last_signal = 0;

    std::signal(SIGHUP, SIG_DFL);
    std::signal(SIGINT, SIG_DFL);
    std::signal(SIGTERM, SIG_DFL);
    std::signal(SIGQUIT, SIG_DFL);

    std::raise(sig);
  }
}

}  // namespace SignalHandler

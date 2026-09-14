#pragma once

#include <functional>
#include <string>
#include <vector>

#include <jack/jack.h>
#include <jack/midiport.h>

#include "aelkey_state.h"
#include "dispatcher_timer.h"
#include "singleton.h"
#include "utils/signal.h"

struct AudioEvent {
  std::string id;             // InputDecl/OutputDecl id
  std::vector<uint8_t> data;  // raw float32 bytes
  uint64_t timestamp_us = 0;
  uint32_t frames = 0;
};

struct MidiEvent {
  std::string id;             // InputDecl/OutputDecl id
  std::vector<uint8_t> data;  // raw MIDI bytes
  uint64_t timestamp_us;
};

struct JackPortEvent {
  std::string type;       // "add" or "remove"
  std::string full_name;  // "Client:Port"
  std::string port_type;  // e.g. JACK_DEFAULT_MIDI_TYPE
  unsigned long flags;    // JackPortIsInput / JackPortIsOutput
};

class BackendJack : public Singleton<BackendJack> {
  friend class Singleton<BackendJack>;

 private:
  BackendJack() = default;
  ~BackendJack() = default;

 public:
  void shutdown();

  // Ensure JACK client exists and is active
  bool ensure_client();

  // Accessors (to be phased out from external use where possible)
  jack_client_t *client() const {
    return client_;
  }

  bool set_client_name(const std::string &name);

  const std::string &client_name() const {
    return client_name_;
  }

  // Port management
  jack_port_t *create_port(const std::string &name, const char *type, unsigned long flags);
  void destroy_port(jack_port_t *port);

  // Port helpers
  std::vector<std::string> list_ports(const char *type, unsigned long flags);
  jack_port_t *find_port(const std::string &full_name);
  std::string port_name(jack_port_t *port);
  void *port_buffer(jack_port_t *port, jack_nframes_t nframes);

  // Connection helpers
  bool connect(const std::string &src, const std::string &dst);
  bool disconnect(const std::string &src, const std::string &dst);

  std::vector<std::string> port_connections(jack_port_t *port);

  // MIDI helpers (JACK-domain, not MIDI-domain)
  uint32_t midi_event_count(void *buf);
  bool midi_event_get(jack_midi_event_t &out, void *buf, uint32_t index);
  void midi_clear_buffer(void *buf);
  jack_midi_data_t *midi_event_reserve(void *buf, jack_nframes_t time, size_t size);

  // Signals
  auto subscribe_process(AelkeyUtil::Signal<void(jack_nframes_t)>::Callback cb) {
    return sig_jack_process_.subscribe(std::move(cb));
  }

  auto subscribe_hotplug(AelkeyUtil::Signal<void(const JackPortEvent &)>::Callback cb) {
    return sig_jack_hotplug_.subscribe(std::move(cb));
  }

  auto subscribe_shutdown(AelkeyUtil::Signal<void(void)>::Callback cb) {
    return sig_jack_shutdown_.subscribe(std::move(cb));
  }

  void notify_shutdown() {
    sig_jack_shutdown_.emit();
  }

  bool is_shutdown() {
    auto &state = AelkeyState::instance();
    if (!state.loop_running || state.loop_should_stop) {
      DispatcherCb cb;
      cb.native = [this]() { this->notify_shutdown(); };
      cb.oneshot = true;

      DispatcherTimer::instance().schedule_ns(1, cb);
      return true;
    }
    return false;
  }

 private:
  bool on_init() override;

  static int process_cb(jack_nframes_t nframes, void *arg);
  int process(jack_nframes_t nframes);

  static void port_reg_cb(jack_port_id_t port_id, int registered, void *arg);
  void handle_port_registration(jack_port_id_t port_id, int registered);

 private:
  jack_client_t *client_ = nullptr;
  std::string client_name_;

  // tokens
  AelkeyUtil::Signal<void(void)>::Connection tok_shutdown_;

  // signals
  AelkeyUtil::Signal<void(jack_nframes_t)> sig_jack_process_;
  AelkeyUtil::Signal<void(const JackPortEvent &)> sig_jack_hotplug_;
  AelkeyUtil::Signal<void(void)> sig_jack_shutdown_;
};

struct JackPortRAII {
  jack_port_t *port = nullptr;

  JackPortRAII() = default;
  explicit JackPortRAII(jack_port_t *p) : port(p) {
    if (port) {
      AelkeyState::instance().increment_active_tasks();
    }
  }

  ~JackPortRAII() {
    if (port) {
      BackendJack::instance().destroy_port(port);
      AelkeyState::instance().decrement_active_tasks();
    }
  }

  JackPortRAII(const JackPortRAII &) = delete;
  JackPortRAII &operator=(const JackPortRAII &) = delete;
  JackPortRAII(JackPortRAII &&) noexcept = default;

  JackPortRAII &operator=(JackPortRAII &&other) noexcept {
    if (this != &other) {
      if (port) {
        BackendJack::instance().destroy_port(port);
        AelkeyState::instance().decrement_active_tasks();
      }
      port = other.port;
      other.port = nullptr;
    }
    return *this;
  }

  operator jack_port_t *() const {
    return port;
  }
  jack_port_t *operator->() const {
    return port;
  }
};

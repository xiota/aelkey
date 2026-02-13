#pragma once

#include <functional>
#include <string>
#include <vector>

#include <jack/jack.h>
#include <jack/midiport.h>

#include "singleton.h"
#include "utils/signal.h"

struct JackPortEvent {
  std::string type;       // "add" or "remove"
  std::string full_name;  // "Client:Port"
  std::string port_type;  // e.g. JACK_DEFAULT_MIDI_TYPE
  unsigned long flags;    // JackPortIsInput / JackPortIsOutput
};

class DeviceBackendJack : public Singleton<DeviceBackendJack> {
  friend class Singleton<DeviceBackendJack>;

 private:
  DeviceBackendJack() = default;
  ~DeviceBackendJack();

 public:
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

 public:
  AelkeyUtil::Signal<void(jack_nframes_t)> sig_jack_process_;
  AelkeyUtil::Signal<void(const JackPortEvent &)> sig_jack_hotplug_;

 private:
  static int process_cb(jack_nframes_t nframes, void *arg);
  int process(jack_nframes_t nframes);

  static void port_reg_cb(jack_port_id_t port_id, int registered, void *arg);
  void handle_port_registration(jack_port_id_t port_id, int registered);

 private:
  jack_client_t *client_ = nullptr;
  std::string client_name_;
};

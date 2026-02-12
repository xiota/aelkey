#include "device_backend_jack.h"

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include <jack/jack.h>
#include <jack/midiport.h>
#include <unistd.h>

DeviceBackendJack::~DeviceBackendJack() {
  if (client_) {
    jack_client_close(client_);
    client_ = nullptr;
  }
}

bool DeviceBackendJack::ensure_client() {
  if (client_) {
    return true;
  }

  if (client_name_.empty()) {
    client_name_ = "Aelkey_Jack_" + std::to_string(getpid());
  }

  jack_status_t status{};
  client_ = jack_client_open(client_name_.c_str(), JackNoStartServer, &status);
  if (!client_) {
    std::fprintf(
        stderr, "JACK: failed to open client '%s' (status=0x%x)\n", client_name_.c_str(), status
    );
    return false;
  }

  if (jack_set_process_callback(client_, &DeviceBackendJack::process_cb, this) != 0) {
    std::fprintf(stderr, "JACK: failed to set process callback\n");
    jack_client_close(client_);
    client_ = nullptr;
    return false;
  }

  // Register port registration callback for hotplug-like events
  jack_set_port_registration_callback(client_, &DeviceBackendJack::port_reg_cb, this);

  if (jack_activate(client_) != 0) {
    std::fprintf(stderr, "JACK: failed to activate client '%s'\n", client_name_.c_str());
    jack_client_close(client_);
    client_ = nullptr;
    return false;
  }

  std::fprintf(stderr, "JACK: client '%s' initialized\n", client_name_.c_str());
  return true;
}

bool DeviceBackendJack::set_client_name(const std::string &name) {
  // Cannot change name after JACK client is created
  if (client_) {
    return false;
  }

  client_name_ = name;
  return true;
}

jack_port_t *
DeviceBackendJack::create_port(const std::string &name, const char *type, unsigned long flags) {
  if (!ensure_client()) {
    return nullptr;
  }

  jack_port_t *port = jack_port_register(client_, name.c_str(), type, flags, 0);
  if (!port) {
    std::fprintf(stderr, "JACK: failed to register port '%s'\n", name.c_str());
  }
  return port;
}

void DeviceBackendJack::destroy_port(jack_port_t *port) {
  if (client_ && port) {
    jack_port_unregister(client_, port);
  }
}

std::vector<std::string> DeviceBackendJack::list_ports(const char *type, unsigned long flags) {
  std::vector<std::string> result;

  if (!ensure_client()) {
    return result;
  }

  const char **ports = jack_get_ports(client_, nullptr, type, flags);
  if (!ports) {
    return result;
  }

  for (int i = 0; ports[i]; ++i) {
    result.emplace_back(ports[i]);
  }

  jack_free(ports);
  return result;
}

jack_port_t *DeviceBackendJack::find_port(const std::string &full_name) {
  if (!ensure_client()) {
    return nullptr;
  }
  return jack_port_by_name(client_, full_name.c_str());
}

std::string DeviceBackendJack::port_name(jack_port_t *port) {
  if (!port) {
    return {};
  }
  const char *name = jack_port_name(port);
  return name ? std::string{ name } : std::string{};
}

void *DeviceBackendJack::port_buffer(jack_port_t *port, jack_nframes_t nframes) {
  if (!port) {
    return nullptr;
  }
  return jack_port_get_buffer(port, nframes);
}

bool DeviceBackendJack::connect(const std::string &src, const std::string &dst) {
  if (!ensure_client()) {
    return false;
  }
  if (jack_connect(client_, src.c_str(), dst.c_str()) != 0) {
    return false;
  }
  return true;
}

bool DeviceBackendJack::disconnect(const std::string &src, const std::string &dst) {
  if (!client_) {
    return false;
  }
  if (jack_disconnect(client_, src.c_str(), dst.c_str()) != 0) {
    return false;
  }
  return true;
}

std::vector<std::string> DeviceBackendJack::port_connections(jack_port_t *port) {
  std::vector<std::string> result;

  if (!client_ || !port) {
    return result;
  }

  const char **conns = jack_port_get_all_connections(client_, port);
  if (!conns) {
    return result;
  }

  for (int i = 0; conns[i]; ++i) {
    result.emplace_back(conns[i]);
  }

  jack_free(conns);
  return result;
}

uint32_t DeviceBackendJack::midi_event_count(void *buf) {
  if (!buf) {
    return 0;
  }
  return jack_midi_get_event_count(buf);
}

bool DeviceBackendJack::midi_event_get(jack_midi_event_t &out, void *buf, uint32_t index) {
  if (!buf) {
    return false;
  }
  if (jack_midi_event_get(&out, buf, index) != 0) {
    return false;
  }
  return true;
}

void DeviceBackendJack::midi_clear_buffer(void *buf) {
  if (!buf) {
    return;
  }
  jack_midi_clear_buffer(buf);
}

jack_midi_data_t *
DeviceBackendJack::midi_event_reserve(void *buf, jack_nframes_t time, size_t size) {
  if (!buf) {
    return nullptr;
  }
  return jack_midi_event_reserve(buf, time, size);
}

void DeviceBackendJack::add_rt_callback(RtCallback cb) {
  callbacks_.push_back(std::move(cb));
}

void DeviceBackendJack::add_hotplug_callback(HotplugCallback cb) {
  hotplug_callbacks_.push_back(std::move(cb));
}

int DeviceBackendJack::process_cb(jack_nframes_t nframes, void *arg) {
  auto *self = static_cast<DeviceBackendJack *>(arg);
  return self->process(nframes);
}

int DeviceBackendJack::process(jack_nframes_t nframes) {
  for (auto &cb : callbacks_) {
    cb(nframes);
  }
  return 0;
}

void DeviceBackendJack::port_reg_cb(jack_port_id_t port_id, int registered, void *arg) {
  auto *self = static_cast<DeviceBackendJack *>(arg);
  if (!self) {
    return;
  }
  self->handle_port_registration(port_id, registered);
}

void DeviceBackendJack::handle_port_registration(jack_port_id_t port_id, int registered) {
  if (!client_) {
    return;
  }

  jack_port_t *port = jack_port_by_id(client_, port_id);
  if (!port) {
    return;
  }

  const char *name = jack_port_name(port);
  const char *type = jack_port_type(port);
  unsigned long flags = jack_port_flags(port);

  JackPortEvent ev;
  ev.type = registered ? "add" : "remove";
  ev.full_name = name ? std::string{ name } : std::string{};
  ev.port_type = type ? std::string{ type } : std::string{};
  ev.flags = flags;

  // Fan out to all subscribers
  for (auto &cb : hotplug_callbacks_) {
    cb(ev);
  }
}

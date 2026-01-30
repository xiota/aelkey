#include "device_backend_midi.h"

#include <cstdio>
#include <cstring>

#include <unistd.h>

#include "device_helpers.h"
#include "tick_scheduler.h"

static constexpr size_t MIDI_RINGBUFFER_BYTES = 64 * 1024;

DeviceBackendMidi::~DeviceBackendMidi() {
  if (tick_fd_ >= 0) {
    TickScheduler::instance().unregister_fd(tick_fd_);
    tick_fd_ = -1;
  }

  if (client_) {
    jack_client_close(client_);
    client_ = nullptr;
  }
  if (ring_) {
    jack_ringbuffer_free(ring_);
    ring_ = nullptr;
  }
}

bool DeviceBackendMidi::on_init() {
  if (client_) {
    return true;
  }

  ring_ = jack_ringbuffer_create(MIDI_RINGBUFFER_BYTES);
  if (!ring_) {
    std::fprintf(stderr, "MIDI: failed to create ringbuffer\n");
    return false;
  }

  client_name_ = "Aelkey_" + std::to_string(getpid());

  jack_status_t status{};
  client_ = jack_client_open(client_name_.c_str(), JackNoStartServer, &status);
  if (!client_) {
    std::fprintf(
        stderr,
        "MIDI: failed to open JACK client '%s' (status=0x%x)\n",
        client_name_.c_str(),
        status
    );
    return false;
  }

  jack_set_process_callback(client_, &DeviceBackendMidi::process_cb, this);

  if (jack_activate(client_) != 0) {
    std::fprintf(stderr, "MIDI: failed to activate JACK client\n");
    jack_client_close(client_);
    client_ = nullptr;
    return false;
  }

  return true;
}

std::string DeviceBackendMidi::ensure_client_name(const InputDecl &decl) {
  // If user specified a client and we haven't overridden yet, rename once.
  if (!decl.client.empty() && client_name_ != decl.client) {
    // Only allow this before any ports are registered.
    if (!inputs_.empty()) {
      // Too late to change; ignore.
      return client_name_;
    }

    // Close old client and reopen with new name.
    if (client_) {
      jack_client_close(client_);
      client_ = nullptr;
    }

    jack_status_t status{};
    client_name_ = decl.client;
    client_ = jack_client_open(client_name_.c_str(), JackNoStartServer, &status);
    if (!client_) {
      std::fprintf(
          stderr,
          "MIDI: failed to reopen JACK client '%s' (status=0x%x)\n",
          client_name_.c_str(),
          status
      );
      return client_name_;
    }

    jack_set_process_callback(client_, &DeviceBackendMidi::process_cb, this);

    if (jack_activate(client_) != 0) {
      std::fprintf(stderr, "MIDI: failed to activate JACK client '%s'\n", client_name_.c_str());
      jack_client_close(client_);
      client_ = nullptr;
      return client_name_;
    }
  }

  return client_name_;
}

std::string DeviceBackendMidi::sanitize_port_name(const std::string &name) const {
  std::string out = name;
  for (char &c : out) {
    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-')) {
      c = '_';
    }
  }
  return out;
}

std::string DeviceBackendMidi::make_default_port_name(const InputDecl &decl) const {
  return sanitize_port_name("midi_" + decl.id);
}

bool DeviceBackendMidi::match(const InputDecl &decl, std::string &devnode_out) {
  lazy_init();
  if (!client_) {
    return false;
  }

  if (decl.type != "midi") {
    return false;
  }

  if (decl.name.empty()) {
    return false;
  }

  const char **ports =
      jack_get_ports(client_, nullptr, JACK_DEFAULT_MIDI_TYPE, JackPortIsOutput);
  if (!ports) {
    return false;
  }

  std::string result;

  for (int i = 0; ports[i]; i++) {
    std::string full = ports[i];  // "Client:Port"
    if (match_string(decl.name, full)) {
      result = full;
      break;
    }
  }

  jack_free(ports);

  if (result.empty()) {
    return false;
  }

  devnode_out = "jack:midi:" + result;
  return true;
}

bool DeviceBackendMidi::attach(const std::string &devnode, InputDecl &decl) {
  lazy_init();
  if (!client_) {
    return false;
  }

  if (devnode.rfind("jack:midi:", 0) != 0) {
    std::fprintf(stderr, "MIDI: invalid devnode '%s'\n", devnode.c_str());
    return false;
  }

  // Ensure client name (may reopen client if user specified one)
  ensure_client_name(decl);
  if (!client_) {
    return false;
  }

  std::string src = devnode.substr(std::strlen("jack:midi:"));  // "Client:Port"

  // Determine port name
  std::string port_name =
      decl.port.empty() ? make_default_port_name(decl) : sanitize_port_name(decl.port);

  jack_port_t *in = jack_port_register(
      client_, port_name.c_str(), JACK_DEFAULT_MIDI_TYPE, JackPortIsInput, 0
  );
  if (!in) {
    std::fprintf(stderr, "MIDI: failed to register input port '%s'\n", port_name.c_str());
    return false;
  }

  if (jack_connect(client_, src.c_str(), jack_port_name(in)) != 0) {
    std::fprintf(
        stderr, "MIDI: failed to connect '%s' -> '%s'\n", src.c_str(), jack_port_name(in)
    );
    jack_port_unregister(client_, in);
    return false;
  }

  inputs_[decl.id] = in;
  source_ports_[in] = src;

  decl.devnode = devnode;
  decl.fd = -1;

  // Start periodic pump if not already running
  if (tick_fd_ < 0) {
    TickCb cb;
    cb.native = [this]() { this->pump_messages(); };
    cb.oneshot = false;

    tick_fd_ = TickScheduler::instance().schedule(8, cb);  // 8 ms
    if (tick_fd_ < 0) {
      std::fprintf(stderr, "MIDI: failed to schedule tick\n");
    }
  }

  return true;
}

bool DeviceBackendMidi::detach(const std::string &id) {
  lazy_init();
  if (!client_) {
    return false;
  }

  auto it = inputs_.find(id);
  if (it == inputs_.end()) {
    return false;
  }

  jack_port_t *in = it->second;
  source_ports_.erase(in);
  jack_port_unregister(client_, in);
  inputs_.erase(it);

  // If no more MIDI devices, stop the tick
  if (inputs_.empty() && tick_fd_ >= 0) {
    TickScheduler::instance().unregister_fd(tick_fd_);
    tick_fd_ = -1;
  }

  return true;
}

int DeviceBackendMidi::process_cb(jack_nframes_t nframes, void *arg) {
  auto *self = static_cast<DeviceBackendMidi *>(arg);
  self->process(nframes);
  return 0;
}

void DeviceBackendMidi::process(jack_nframes_t nframes) {
  if (!ring_) {
    return;
  }

  for (auto &[id, port] : inputs_) {
    void *buf = jack_port_get_buffer(port, nframes);
    uint32_t count = jack_midi_get_event_count(buf);

    for (uint32_t i = 0; i < count; i++) {
      jack_midi_event_t ev;
      if (jack_midi_event_get(&ev, buf, i) != 0) {
        continue;
      }

      MidiEvent me;
      me.id = id;
      me.data.assign(ev.buffer, ev.buffer + ev.size);

      push_event(me);
    }
  }
}

void DeviceBackendMidi::push_event(const MidiEvent &ev) {
  if (!ring_) {
    return;
  }

  // Serialize: [uint32_t size][uint32_t id_len][id bytes][data bytes]
  uint32_t size = static_cast<uint32_t>(ev.data.size());
  uint32_t id_len = static_cast<uint32_t>(ev.id.size());

  size_t total = sizeof(size) + sizeof(id_len) + id_len + size;
  if (jack_ringbuffer_write_space(ring_) < total) {
    // Drop event on overflow
    return;
  }

  jack_ringbuffer_write(ring_, reinterpret_cast<const char *>(&size), sizeof(size));
  jack_ringbuffer_write(ring_, reinterpret_cast<const char *>(&id_len), sizeof(id_len));
  if (id_len) {
    jack_ringbuffer_write(ring_, ev.id.data(), id_len);
  }
  if (size) {
    jack_ringbuffer_write(ring_, reinterpret_cast<const char *>(ev.data.data()), size);
  }
}

bool DeviceBackendMidi::pop_event(MidiEvent &out) {
  if (!ring_) {
    return false;
  }

  if (jack_ringbuffer_read_space(ring_) < sizeof(uint32_t) * 2) {
    return false;
  }

  uint32_t size = 0;
  uint32_t id_len = 0;

  jack_ringbuffer_read(ring_, reinterpret_cast<char *>(&size), sizeof(size));
  jack_ringbuffer_read(ring_, reinterpret_cast<char *>(&id_len), sizeof(id_len));

  if (jack_ringbuffer_read_space(ring_) < id_len + size) {
    // Corrupt / partial; drop
    return false;
  }

  out.id.resize(id_len);
  if (id_len) {
    jack_ringbuffer_read(ring_, out.id.data(), id_len);
  }

  out.data.resize(size);
  if (size) {
    jack_ringbuffer_read(ring_, reinterpret_cast<char *>(out.data.data()), size);
  }

  return true;
}

void DeviceBackendMidi::dispatch_to_lua(const MidiEvent &ev) {
  auto &state = AelkeyState::instance();
  sol::state_view lua(state.lua_vm);

  auto it = state.input_map.find(ev.id);
  if (it == state.input_map.end()) {
    return;
  }

  const InputDecl &decl = it->second;
  if (decl.on_event.empty()) {
    return;
  }

  sol::object obj = lua[decl.on_event];
  if (!obj.is<sol::function>()) {
    return;
  }

  sol::function cb = obj.as<sol::function>();

  sol::table tbl = lua.create_table();
  tbl["device"] = decl.id;
  tbl["status"] = "ok";
  tbl["size"] = static_cast<int>(ev.data.size());
  tbl["data"] =
      std::string_view(reinterpret_cast<const char *>(ev.data.data()), ev.data.size());

  sol::protected_function pf = cb;
  sol::protected_function_result res = pf(tbl);
  if (!res.valid()) {
    sol::error err = res;
    std::fprintf(stderr, "Lua MIDI callback error: %s\n", err.what());
  }
}

void DeviceBackendMidi::pump_messages() {
  MidiEvent ev;
  while (pop_event(ev)) {
    dispatch_to_lua(ev);
  }
}

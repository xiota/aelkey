#include "device_out_midi.h"

#include <cstdio>
#include <cstring>
#include <unistd.h>

bool DeviceOutMidi::on_init() {
  if (!client_name_.empty()) {
    // client_name_ may have been set by ensure_client_name() before lazy_init()
  } else {
    client_name_ = "Aelkey_MidiOut_" + std::to_string(getpid());
  }

  jack_status_t status{};
  client_ = jack_client_open(client_name_.c_str(), JackNullOption, &status);
  if (!client_) {
    std::fprintf(
        stderr,
        "MIDI OUT: failed to open JACK client '%s' (status=0x%x)\n",
        client_name_.c_str(),
        status
    );
    return false;
  }

  if (jack_set_process_callback(client_, &DeviceOutMidi::process_cb, this) != 0) {
    std::fprintf(stderr, "MIDI OUT: failed to set process callback\n");
    jack_client_close(client_);
    client_ = nullptr;
    return false;
  }

  ring_ = jack_ringbuffer_create(kRingSize);
  if (!ring_) {
    std::fprintf(stderr, "MIDI OUT: failed to create ringbuffer\n");
    jack_client_close(client_);
    client_ = nullptr;
    return false;
  }

  jack_ringbuffer_mlock(ring_);

  if (jack_activate(client_) != 0) {
    std::fprintf(stderr, "MIDI OUT: failed to activate JACK client\n");
    jack_client_close(client_);
    client_ = nullptr;
    jack_ringbuffer_free(ring_);
    ring_ = nullptr;
    return false;
  }

  std::fprintf(stderr, "MIDI OUT: JACK client '%s' initialized\n", client_name_.c_str());
  return true;
}

DeviceOutMidi::~DeviceOutMidi() {
  if (client_) {
    jack_client_close(client_);
    client_ = nullptr;
  }
  if (ring_) {
    jack_ringbuffer_free(ring_);
    ring_ = nullptr;
  }
}

bool DeviceOutMidi::ensure_client_name(const OutputDecl &decl) {
  // If client not yet created, allow overriding client_name_ once.
  if (!client_) {
    if (!decl.client.empty()) {
      client_name_ = decl.client;
    }
    return lazy_init();
  }

  // Already initialized: if a different name is requested, ignore it.
  if (!decl.client.empty() && decl.client != client_name_) {
    std::fprintf(
        stderr,
        "MIDI OUT: client already initialized as '%s', ignoring requested '%s'\n",
        client_name_.c_str(),
        decl.client.c_str()
    );
  }
  return true;
}

bool DeviceOutMidi::create(const OutputDecl &decl) {
  if (!ensure_client_name(decl)) {
    return false;
  }

  // Port name: decl.port or "<id>"
  std::string port_name;
  if (!decl.port.empty()) {
    port_name = decl.port;
  } else {
    port_name = decl.id;
  }

  // Ensure uniqueness per client.
  for (const auto &kv : outputs_) {
    if (std::strcmp(jack_port_name(kv.second), port_name.c_str()) == 0) {
      std::fprintf(
          stderr,
          "MIDI OUT: port '%s' already exists, reusing for id '%s'\n",
          port_name.c_str(),
          decl.id.c_str()
      );
      outputs_[decl.id] = kv.second;
      return true;
    }
  }

  jack_port_t *out = jack_port_register(
      client_, port_name.c_str(), JACK_DEFAULT_MIDI_TYPE, JackPortIsOutput, 0
  );
  if (!out) {
    std::fprintf(stderr, "MIDI OUT: failed to register output port '%s'\n", port_name.c_str());
    return false;
  }

  outputs_[decl.id] = out;

  // Auto-connect if name is provided: connect to ALL matching JACK MIDI inputs.
  if (!decl.name.empty()) {
    const char **ports =
        jack_get_ports(client_, nullptr, JACK_DEFAULT_MIDI_TYPE, JackPortIsInput);
    if (!ports) {
      std::fprintf(stderr, "MIDI OUT: no JACK MIDI input ports found for auto-connect\n");
    } else {
      const char *src = jack_port_name(out);
      int connected = 0;

      for (int i = 0; ports[i]; ++i) {
        std::string full = ports[i];  // "Client:Port"
        if (match_string(decl.name, full)) {
          if (jack_connect(client_, src, full.c_str()) == 0) {
            ++connected;
            std::fprintf(stderr, "MIDI OUT: connected '%s' -> '%s'\n", src, full.c_str());
          } else {
            std::fprintf(
                stderr, "MIDI OUT: failed to connect '%s' -> '%s'\n", src, full.c_str()
            );
          }
        }
      }

      if (connected == 0) {
        std::fprintf(
            stderr, "MIDI OUT: no JACK MIDI inputs matched pattern '%s'\n", decl.name.c_str()
        );
      }

      jack_free(ports);
    }
  }

  return true;
}

bool DeviceOutMidi::send(const std::string &id, const uint8_t *data, size_t len) {
  if (!client_ || !ring_) {
    return false;
  }

  auto it = outputs_.find(id);
  if (it == outputs_.end()) {
    // Unknown id; nothing to send to.
    return false;
  }

  if (len == 0 || len > 3) {
    // For now, only support up to 3-byte messages (no SysEx).
    // Extend format later if needed.
    return false;
  }

  uint8_t msg_len = static_cast<uint8_t>(len);
  uint8_t id_len = static_cast<uint8_t>(id.size());
  size_t total = 1 + 1 + id_len + msg_len;

  if (jack_ringbuffer_write_space(ring_) < total) {
    // Overflow: drop event (non-blocking, RT-safe behavior).
    return false;
  }

  jack_ringbuffer_write(ring_, reinterpret_cast<const char *>(&msg_len), 1);
  jack_ringbuffer_write(ring_, reinterpret_cast<const char *>(&id_len), 1);
  if (id_len > 0) {
    jack_ringbuffer_write(ring_, id.data(), id_len);
  }
  jack_ringbuffer_write(ring_, reinterpret_cast<const char *>(data), msg_len);

  return true;
}

bool DeviceOutMidi::destroy(const std::string &id) {
  auto it = outputs_.find(id);
  if (it == outputs_.end()) {
    return false;
  }

  if (client_) {
    jack_port_unregister(client_, it->second);
  }
  outputs_.erase(it);
  return true;
}

int DeviceOutMidi::process_cb(jack_nframes_t nframes, void *arg) {
  auto *self = static_cast<DeviceOutMidi *>(arg);
  self->process(nframes);
  return 0;
}

void DeviceOutMidi::process(jack_nframes_t nframes) {
  if (!client_) {
    return;
  }

  // Clear all output buffers.
  for (auto &kv : outputs_) {
    void *buf = jack_port_get_buffer(kv.second, nframes);
    jack_midi_clear_buffer(buf);
  }

  if (!ring_) {
    return;
  }

  // Drain ringbuffer.
  while (true) {
    if (jack_ringbuffer_read_space(ring_) < 2) {
      break;
    }

    uint8_t len = 0;
    uint8_t id_len = 0;

    jack_ringbuffer_read(ring_, reinterpret_cast<char *>(&len), 1);
    jack_ringbuffer_read(ring_, reinterpret_cast<char *>(&id_len), 1);

    if (len == 0 || len > 3) {
      // Corrupt/unsupported; try to skip payload if present.
      size_t skip = id_len + len;
      if (jack_ringbuffer_read_space(ring_) >= skip) {
        jack_ringbuffer_read_advance(ring_, skip);
      }
      continue;
    }

    std::string id;
    id.resize(id_len);
    if (id_len > 0) {
      jack_ringbuffer_read(ring_, id.data(), id_len);
    }

    uint8_t data[3] = { 0, 0, 0 };
    jack_ringbuffer_read(ring_, reinterpret_cast<char *>(data), len);

    auto it = outputs_.find(id);
    if (it == outputs_.end()) {
      // Port no longer exists; drop.
      continue;
    }

    void *buf = jack_port_get_buffer(it->second, nframes);
    jack_midi_data_t *dst = jack_midi_event_reserve(buf, 0, len);
    if (!dst) {
      // Buffer full; drop.
      continue;
    }
    std::memcpy(dst, data, len);
  }
}

#include "device_out_midi.h"

#include <cstdio>
#include <cstring>
#include <string>

#include <jack/ringbuffer.h>

#include "device_backend_jack.h"
#include "device_helpers.h"
#include "tick_scheduler.h"

bool DeviceOutMidi::on_init() {
  if (ring_) {
    return true;
  }

  ring_ = jack_ringbuffer_create(kRingSize);
  if (!ring_) {
    std::fprintf(stderr, "MIDI OUT: failed to create ringbuffer\n");
    return false;
  }

  jack_ringbuffer_mlock(ring_);

  auto &jack = DeviceBackendJack::instance();
  if (!rt_registered_) {
    rt_cb_ = [this](jack_nframes_t n) { this->process(n); };
    jack.add_rt_callback(rt_cb_);
    rt_registered_ = true;
  }

  if (!hotplug_registered_) {
    hp_cb_ = [this](const JackPortEvent &ev) { this->on_hotplug_event(ev); };
    jack.add_hotplug_callback(hp_cb_);
    hotplug_registered_ = true;
  }

  return true;
}

DeviceOutMidi::~DeviceOutMidi() {
  auto &jack = DeviceBackendJack::instance();

  for (auto &kv : output_ports_) {
    jack.destroy_port(kv.second);
  }
  output_ports_.clear();
  output_decls_.clear();

  if (rt_registered_) {
    jack.remove_rt_callback(rt_cb_);
    rt_registered_ = false;
  }

  if (hotplug_registered_) {
    jack.remove_hotplug_callback(hp_cb_);
    hotplug_registered_ = false;
  }

  if (ring_) {
    jack_ringbuffer_free(ring_);
    ring_ = nullptr;
  }
}

bool DeviceOutMidi::create(const OutputDecl &decl) {
  if (!lazy_init()) {
    return false;
  }

  // No sanitization — use exactly what user provided
  std::string port_name = decl.port.empty() ? decl.id : decl.port;

  // Reuse existing port if name matches
  for (const auto &kv : output_ports_) {
    auto existing_name = DeviceBackendJack::instance().port_name(kv.second);
    if (existing_name == port_name) {
      output_ports_[decl.id] = kv.second;
      output_decls_[decl.id] = decl;
      return true;
    }
  }

  auto &jack = DeviceBackendJack::instance();
  jack_port_t *out = jack.create_port(port_name, JACK_DEFAULT_MIDI_TYPE, JackPortIsOutput);

  if (!out) {
    std::fprintf(stderr, "MIDI OUT: failed to register output port '%s'\n", port_name.c_str());
    return false;
  }

  output_ports_[decl.id] = out;
  output_decls_[decl.id] = decl;

  // Auto-connect if user provided a pattern
  if (!decl.name.empty()) {
    auto ports = jack.list_ports(JACK_DEFAULT_MIDI_TYPE, JackPortIsInput);
    std::string src = jack.port_name(out);

    for (auto &full : ports) {
      if (match_string(decl.name, full)) {
        if (!jack.connect(src, full)) {
          std::fprintf(
              stderr, "MIDI OUT: failed to connect '%s' -> '%s'\n", src.c_str(), full.c_str()
          );
        } else {
          std::fprintf(stderr, "MIDI OUT: connected '%s' -> '%s'\n", src.c_str(), full.c_str());
        }
      }
    }
  }

  return true;
}

bool DeviceOutMidi::send(const std::string &id, const uint8_t *data, size_t len) {
  if (!ring_) {
    return false;
  }

  auto it = output_ports_.find(id);
  if (it == output_ports_.end()) {
    return false;
  }

  if (len == 0 || len > 3) {
    return false;
  }

  uint8_t msg_len = static_cast<uint8_t>(len);
  uint8_t id_len = static_cast<uint8_t>(id.size());
  size_t total = 1 + 1 + id_len + msg_len;

  if (jack_ringbuffer_write_space(ring_) < total) {
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
  auto it = output_ports_.find(id);
  if (it == output_ports_.end()) {
    return false;
  }

  DeviceBackendJack::instance().destroy_port(it->second);
  output_ports_.erase(it);

  auto it2 = output_decls_.find(id);
  if (it2 != output_decls_.end()) {
    output_decls_.erase(it2);
  }

  return true;
}

void DeviceOutMidi::process(jack_nframes_t nframes) {
  auto &jack = DeviceBackendJack::instance();

  // Clear all output buffers
  for (auto &kv : output_ports_) {
    void *buf = jack.port_buffer(kv.second, nframes);
    jack.midi_clear_buffer(buf);
  }

  if (!ring_) {
    return;
  }

  while (true) {
    if (jack_ringbuffer_read_space(ring_) < 2) {
      break;
    }

    uint8_t len = 0;
    uint8_t id_len = 0;

    jack_ringbuffer_read(ring_, reinterpret_cast<char *>(&len), 1);
    jack_ringbuffer_read(ring_, reinterpret_cast<char *>(&id_len), 1);

    if (len == 0 || len > 3) {
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

    auto it = output_ports_.find(id);
    if (it == output_ports_.end()) {
      continue;
    }

    void *buf = jack.port_buffer(it->second, nframes);
    jack_midi_data_t *dst = jack.midi_event_reserve(buf, 0, len);
    if (!dst) {
      continue;
    }

    std::memcpy(dst, data, len);
  }
}

void DeviceOutMidi::on_hotplug_event(const JackPortEvent &ev) {
  // Only care about MIDI input ports (destinations)
  if (ev.port_type != JACK_DEFAULT_MIDI_TYPE) {
    return;
  }
  if (!(ev.flags & JackPortIsInput)) {
    return;
  }

  pending_hotplug_.push_back(ev);

  // Schedule a one-shot tick to process them
  TickCb cb;
  cb.native = [this]() { this->process_hotplug_events(); };
  cb.oneshot = true;

  TickScheduler::instance().schedule(4, cb);
}

void DeviceOutMidi::process_hotplug_events() {
  auto &jack = DeviceBackendJack::instance();

  // For each OutputDecl
  for (auto &[id, decl] : output_decls_) {
    if (decl.type != "midi") {
      continue;
    }

    // Skip virtual ports (decl.name empty)
    if (decl.name.empty()) {
      continue;
    }

    // Get our internal JACK port
    auto it = output_ports_.find(id);
    if (it == output_ports_.end()) {
      continue;
    }

    jack_port_t *internal = it->second;
    std::string src = jack.port_name(internal);

    // For each pending event
    for (auto &ev : pending_hotplug_) {
      if (ev.type == "add") {
        // Does this external port match the user pattern?
        if (match_string(decl.name, ev.full_name)) {
          // Connect external -> internal
          if (jack.connect(src, ev.full_name)) {
            std::fprintf(
                stderr,
                "MIDI OUT hotplug: connected '%s' -> '%s'\n",
                src.c_str(),
                ev.full_name.c_str()
            );
          }
        }
      }
    }
  }

  pending_hotplug_.clear();
}

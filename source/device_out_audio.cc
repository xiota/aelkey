#include "device_out_audio.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <jack/ringbuffer.h>

#include "device_backend_jack.h"
#include "device_helpers.h"
#include "tick_scheduler.h"
#include "utils/signal.h"

bool DeviceOutAudio::on_init() {
  if (ring_) {
    return true;
  }

  ring_ = jack_ringbuffer_create(kRingSize);
  if (!ring_) {
    std::fprintf(stderr, "AUDIO OUT: failed to create ringbuffer\n");
    return false;
  }

  auto &jack = DeviceBackendJack::instance();
  tok_jack_process_ =
      jack.sig_jack_process_.subscribe([this](jack_nframes_t nframes) { process(nframes); });

  tok_jack_hotplug_ = jack.sig_jack_hotplug_.subscribe([this](const JackPortEvent &ev) {
    on_hotplug_event(ev);
  });

  return true;
}

DeviceOutAudio::~DeviceOutAudio() {
  auto &jack = DeviceBackendJack::instance();

  for (auto &kv : output_ports_) {
    jack.destroy_port(kv.second);
  }
  output_ports_.clear();
  output_decls_.clear();

  if (ring_) {
    jack_ringbuffer_free(ring_);
    ring_ = nullptr;
  }
}

bool DeviceOutAudio::create(const OutputDecl &decl) {
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
      return true;
    }
  }

  auto &jack = DeviceBackendJack::instance();
  jack_port_t *out = jack.create_port(port_name, JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput);

  if (!out) {
    std::fprintf(stderr, "AUDIO OUT: failed to register output port '%s'\n", port_name.c_str());
    return false;
  }

  output_ports_[decl.id] = out;
  output_decls_[decl.id] = decl;

  // Auto-connect if user provided a pattern
  if (!decl.name.empty()) {
    auto ports = jack.list_ports(JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput);
    std::string src = jack.port_name(out);

    for (auto &full : ports) {
      if (match_string(decl.name, full)) {
        if (!jack.connect(src, full)) {
          std::fprintf(
              stderr, "AUDIO OUT: failed to connect '%s' -> '%s'\n", src.c_str(), full.c_str()
          );
        } else {
          std::fprintf(
              stderr, "AUDIO OUT: connected '%s' -> '%s'\n", src.c_str(), full.c_str()
          );
        }
      }
    }
  }

  return true;
}

bool DeviceOutAudio::send(const std::string &id, const float *samples, size_t frames) {
  if (!ring_) {
    return false;
  }

  auto it = output_ports_.find(id);
  if (it == output_ports_.end()) {
    return false;
  }

  if (!samples || frames == 0) {
    return false;
  }

  uint32_t frames_u32 = static_cast<uint32_t>(frames);
  uint32_t id_len = static_cast<uint32_t>(id.size());
  uint32_t data_bytes = frames_u32 * sizeof(float);

  // frames + id_len + id + data
  size_t total = sizeof(frames_u32) + sizeof(id_len) + id_len + data_bytes;

  if (jack_ringbuffer_write_space(ring_) < total) {
    return false;
  }

  jack_ringbuffer_write(ring_, reinterpret_cast<const char *>(&frames_u32), sizeof(frames_u32));
  jack_ringbuffer_write(ring_, reinterpret_cast<const char *>(&id_len), sizeof(id_len));

  if (id_len > 0) {
    jack_ringbuffer_write(ring_, id.data(), id_len);
  }

  if (data_bytes > 0) {
    jack_ringbuffer_write(ring_, reinterpret_cast<const char *>(samples), data_bytes);
  }

  return true;
}

bool DeviceOutAudio::destroy(const std::string &id) {
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

void DeviceOutAudio::process(jack_nframes_t nframes) {
  auto &jack = DeviceBackendJack::instance();

  // Clear all output buffers initially (silence by default)
  for (auto &kv : output_ports_) {
    void *buf = jack.port_buffer(kv.second, nframes);
    if (buf) {
      std::memset(buf, 0, sizeof(float) * nframes);
    }
  }

  if (!ring_) {
    return;
  }

  // For each JACK callback, try to consume as many packets as possible.
  // Each packet targets a specific output id and a mono block of samples.
  while (true) {
    // Need at least frames + id_len
    if (jack_ringbuffer_read_space(ring_) < sizeof(uint32_t) * 2) {
      break;
    }

    uint32_t frames = 0;
    uint32_t id_len = 0;

    jack_ringbuffer_read(ring_, reinterpret_cast<char *>(&frames), sizeof(frames));
    jack_ringbuffer_read(ring_, reinterpret_cast<char *>(&id_len), sizeof(id_len));

    size_t data_bytes = static_cast<size_t>(frames) * sizeof(float);

    // Check if the rest of the packet is available
    if (jack_ringbuffer_read_space(ring_) < id_len + data_bytes) {
      break;
    }

    std::string id;
    id.resize(id_len);
    if (id_len > 0) {
      jack_ringbuffer_read(ring_, id.data(), id_len);
    }

    std::vector<float> samples;
    samples.resize(frames);
    if (data_bytes > 0) {
      jack_ringbuffer_read(ring_, reinterpret_cast<char *>(samples.data()), data_bytes);
    }

    auto it = output_ports_.find(id);
    if (it == output_ports_.end()) {
      continue;
    }

    void *buf = jack.port_buffer(it->second, nframes);
    if (!buf) {
      continue;
    }

    float *out = reinterpret_cast<float *>(buf);

    // Zero-pad or truncate to match nframes
    if (frames == nframes) {
      std::memcpy(out, samples.data(), data_bytes);
    } else if (frames < nframes) {
      // Copy what we have, leave the rest as zero (already cleared)
      std::memcpy(out, samples.data(), frames * sizeof(float));
      // remaining samples are already zero from initial clear
    } else {  // frames > nframes
      // Truncate
      std::memcpy(out, samples.data(), nframes * sizeof(float));
    }
  }
}

void DeviceOutAudio::on_hotplug_event(const JackPortEvent &ev) {
  // Only care about audio input ports (destinations)
  if (ev.port_type != JACK_DEFAULT_AUDIO_TYPE) {
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

void DeviceOutAudio::process_hotplug_events() {
  auto &jack = DeviceBackendJack::instance();

  // For each OutputDecl
  for (auto &[id, decl] : output_decls_) {
    if (decl.type != "audio") {
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
                "AUDIO OUT hotplug: connected '%s' -> '%s'\n",
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

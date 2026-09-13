#include "device_out_midi.h"

#include <cstdio>
#include <cstring>
#include <string>

#include "backend_jack.h"
#include "dispatcher.h"
#include "tick_scheduler.h"
#include "utils/regex_match.h"
#include "utils/signal.h"

bool DeviceOutMidi::on_init() {
  auto &jack = BackendJack::instance();
  tok_jack_process_ =
      jack.sig_jack_process_.subscribe([this](jack_nframes_t nframes) { process(nframes); });

  tok_jack_hotplug_ = jack.sig_jack_hotplug_.subscribe([this](const JackPortEvent &ev) {
    on_hotplug_event(ev);
  });

  return true;
}

DeviceOutMidi::~DeviceOutMidi() {
  auto &jack = BackendJack::instance();

  for (auto &kv : output_ports_) {
    jack.destroy_port(kv.second);
  }
  output_ports_.clear();
  output_decls_.clear();
}

bool DeviceOutMidi::create(const OutputDecl &decl) {
  if (!lazy_init()) {
    return false;
  }

  // No sanitization — use exactly what user provided
  std::string port_name = decl.port.empty() ? decl.id : decl.port;

  // Reuse existing port if name matches
  for (const auto &kv : output_ports_) {
    auto existing_name = BackendJack::instance().port_name(kv.second);
    if (existing_name == port_name) {
      output_ports_[decl.id] = kv.second;
      output_decls_[decl.id] = decl;
      return true;
    }
  }

  auto &jack = BackendJack::instance();
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
      if (AelkeyUtil::match_string(decl.name, full)) {
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
  auto it = output_ports_.find(id);
  if (it == output_ports_.end()) {
    return false;
  }

  if (len == 0 || len > 3) {
    return false;
  }

  MidiEvent me;
  me.id = id;
  me.data.assign(data, data + len);

  return queue_.enqueue(me);
}

bool DeviceOutMidi::destroy(const std::string &id) {
  auto it = output_ports_.find(id);
  if (it == output_ports_.end()) {
    return false;
  }

  BackendJack::instance().destroy_port(it->second);
  output_ports_.erase(it);

  auto it2 = output_decls_.find(id);
  if (it2 != output_decls_.end()) {
    output_decls_.erase(it2);
  }

  return true;
}

void DeviceOutMidi::process(jack_nframes_t nframes) {
  auto &jack = BackendJack::instance();

  // Clear all output buffers
  for (auto &kv : output_ports_) {
    void *buf = jack.port_buffer(kv.second, nframes);
    jack.midi_clear_buffer(buf);
  }

  MidiEvent me;
  while (queue_.try_dequeue(me)) {
    auto it = output_ports_.find(me.id);
    if (it == output_ports_.end()) {
      continue;
    }

    void *buf = jack.port_buffer(it->second, nframes);
    jack_midi_data_t *dst = jack.midi_event_reserve(buf, 0, me.data.size());
    if (!dst) {
      continue;
    }

    std::memcpy(dst, me.data.data(), me.data.size());
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
  DispatcherCb cb;
  cb.native = [this]() { this->process_hotplug_events(); };
  cb.oneshot = true;

  TickScheduler::instance().schedule(4, cb);
}

void DeviceOutMidi::process_hotplug_events() {
  auto &jack = BackendJack::instance();

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
        if (AelkeyUtil::match_string(decl.name, ev.full_name)) {
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

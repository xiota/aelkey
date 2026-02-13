#include "device_in_audio.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include <jack/ringbuffer.h>

#include "device_backend_jack.h"
#include "device_helpers.h"
#include "tick_scheduler.h"
#include "utils/signal.h"
#include "utils/time.h"

DeviceInAudio::~DeviceInAudio() {
  if (tick_fd_ >= 0) {
    TickScheduler::instance().unregister_fd(tick_fd_);
    tick_fd_ = -1;
  }

  auto &jack = DeviceBackendJack::instance();
  for (auto &kv : input_ports_) {
    jack.destroy_port(kv.second);
  }

  input_ports_.clear();
  input_decls_.clear();

  if (ring_) {
    jack_ringbuffer_free(ring_);
    ring_ = nullptr;
  }
}

bool DeviceInAudio::on_init() {
  if (ring_) {
    return true;
  }

  ring_ = jack_ringbuffer_create(AUDIO_RINGBUFFER_BYTES);
  if (!ring_) {
    std::fprintf(stderr, "AUDIO: failed to create ringbuffer\n");
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

bool DeviceInAudio::match(InputDecl &decl, std::string &devnode_out) {
  if (!lazy_init()) {
    return false;
  }

  if (decl.type != "audio") {
    return false;
  }

  // unused, always create port
  devnode_out = "jack:audio:" + decl.id;
  return true;
}

bool DeviceInAudio::attach(const std::string &devnode, InputDecl &decl) {
  if (!lazy_init()) {
    return false;
  }

  // No sanitization — use exactly what user provided
  std::string port_name = decl.port.empty() ? decl.id : decl.port;

  auto &jack = DeviceBackendJack::instance();
  jack_port_t *in = jack.create_port(port_name, JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput);
  if (!in) {
    std::fprintf(stderr, "AUDIO: failed to register input port '%s'\n", port_name.c_str());
    return false;
  }

  if (!decl.name.empty()) {
    std::string dst = jack.port_name(in);

    auto ports = jack.list_ports(JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput);
    for (auto &full : ports) {
      if (match_string(decl.name, full)) {
        if (!jack.connect(full, dst)) {
          std::fprintf(
              stderr, "AUDIO: failed to connect '%s' -> '%s'\n", full.c_str(), dst.c_str()
          );
        } else {
          std::fprintf(stderr, "AUDIO: connected '%s' -> '%s'\n", full.c_str(), dst.c_str());
        }
      }
    }
  }

  input_ports_[decl.id] = in;
  input_decls_[decl.id] = decl;

  decl.devnode = devnode;  // unused
  decl.fd = -1;

  if (tick_fd_ < 0) {
    TickCb cb;
    cb.native = [this]() { this->pump_messages(); };
    cb.oneshot = false;

    tick_fd_ = TickScheduler::instance().schedule(8, cb);
    if (tick_fd_ < 0) {
      std::fprintf(stderr, "AUDIO: failed to schedule tick\n");
    }
  }

  return true;
}

bool DeviceInAudio::detach(const std::string &id) {
  if (!lazy_init()) {
    return false;
  }

  auto it = input_ports_.find(id);
  if (it == input_ports_.end()) {
    return false;
  }

  auto &jack = DeviceBackendJack::instance();
  jack.destroy_port(it->second);
  input_ports_.erase(it);

  if (input_ports_.empty() && tick_fd_ >= 0) {
    TickScheduler::instance().unregister_fd(tick_fd_);
    tick_fd_ = -1;
  }

  auto it2 = input_decls_.find(id);
  if (it2 != input_decls_.end()) {
    input_decls_.erase(it2);
  }

  return true;
}

void DeviceInAudio::process(jack_nframes_t nframes) {
  if (!ring_) {
    return;
  }

  auto &jack = DeviceBackendJack::instance();

  for (auto &[id, port] : input_ports_) {
    void *buf = jack.port_buffer(port, nframes);
    if (!buf) {
      continue;
    }

    AudioEvent ev;
    ev.id = id;
    ev.frames = static_cast<uint32_t>(nframes);
    ev.timestamp_us = AelkeyUtil::now("us");

    size_t bytes = static_cast<size_t>(nframes) * sizeof(float);
    ev.data.resize(bytes);
    std::memcpy(ev.data.data(), buf, bytes);

    push_event(ev);
  }
}

void DeviceInAudio::push_event(const AudioEvent &ev) {
  if (!ring_) {
    return;
  }

  uint32_t frames = ev.frames;
  uint32_t id_len = static_cast<uint32_t>(ev.id.size());
  uint32_t data_size = static_cast<uint32_t>(ev.data.size());
  uint64_t timestamp_us = ev.timestamp_us;

  // frames + id_len + id + timestamp + data
  size_t total = sizeof(frames) + sizeof(id_len) + id_len + sizeof(timestamp_us) + data_size;

  if (jack_ringbuffer_write_space(ring_) < total) {
    return;
  }

  jack_ringbuffer_write(ring_, reinterpret_cast<const char *>(&frames), sizeof(frames));
  jack_ringbuffer_write(ring_, reinterpret_cast<const char *>(&id_len), sizeof(id_len));

  if (id_len) {
    jack_ringbuffer_write(ring_, ev.id.data(), id_len);
  }

  jack_ringbuffer_write(
      ring_, reinterpret_cast<const char *>(&timestamp_us), sizeof(timestamp_us)
  );

  if (data_size) {
    jack_ringbuffer_write(ring_, reinterpret_cast<const char *>(ev.data.data()), data_size);
  }
}

bool DeviceInAudio::pop_event(AudioEvent &out) {
  if (!ring_) {
    return false;
  }

  // Need at least the header
  if (jack_ringbuffer_read_space(ring_) < sizeof(uint32_t) * 2) {
    return false;
  }

  // Peek first
  uint32_t header[2];
  jack_ringbuffer_peek(ring_, reinterpret_cast<char *>(header), sizeof(header));

  uint32_t frames = header[0];
  uint32_t id_len = header[1];

  // Check if full message is available
  size_t data_size = static_cast<size_t>(frames) * sizeof(float);

  size_t total_needed =
      sizeof(uint32_t) + sizeof(uint32_t) + id_len + sizeof(uint64_t) + data_size;

  if (jack_ringbuffer_read_space(ring_) < total_needed) {
    return false;
  }

  // Safe to read
  jack_ringbuffer_read(ring_, reinterpret_cast<char *>(&frames), sizeof(frames));
  jack_ringbuffer_read(ring_, reinterpret_cast<char *>(&id_len), sizeof(id_len));

  out.frames = frames;

  out.id.resize(id_len);
  if (id_len) {
    jack_ringbuffer_read(ring_, out.id.data(), id_len);
  }

  jack_ringbuffer_read(ring_, reinterpret_cast<char *>(&out.timestamp_us), sizeof(uint64_t));

  out.data.resize(data_size);
  if (data_size) {
    jack_ringbuffer_read(ring_, reinterpret_cast<char *>(out.data.data()), data_size);
  }

  return true;
}

void DeviceInAudio::dispatch_batch_to_lua(
    const std::string &callback_name,
    const std::vector<AudioEvent> &events
) {
  auto &state = AelkeyState::instance();
  sol::state_view lua(state.lua_vm);

  sol::object obj = lua[callback_name];
  if (!obj.is<sol::function>()) {
    return;
  }

  sol::function cb = obj.as<sol::function>();

  sol::table list = lua.create_table();
  int idx = 1;

  for (auto &ev : events) {
    sol::table e = lua.create_table();

    e["device"] = ev.id;
    e["timestamp"] = ev.timestamp_us;
    e["data"] =
        std::string_view(reinterpret_cast<const char *>(ev.data.data()), ev.data.size());
    e["size"] = static_cast<int>(ev.data.size());
    e["frames"] = static_cast<int>(ev.frames);
    e["status"] = "ok";

    list[idx++] = e;
  }

  sol::protected_function pf = cb;
  sol::protected_function_result res = pf(list);

  if (!res.valid()) {
    sol::error err = res;
    std::fprintf(stderr, "Lua AUDIO batch callback error: %s\n", err.what());
  }
}

void DeviceInAudio::pump_messages() {
  AudioEvent ev;
  while (pop_event(ev)) {
    // Look up InputDecl to find callback name
    auto it_decl = input_decls_.find(ev.id);
    if (it_decl == input_decls_.end()) {
      continue;
    }

    const InputDecl &decl = it_decl->second;
    if (decl.on_event.empty()) {
      continue;
    }

    // batch by callback name
    batches_[decl.on_event].push_back(ev);
  }

  for (auto &[cb, batch] : batches_) {
    if (!batch.empty()) {
      dispatch_batch_to_lua(cb, batch);
      batch.clear();
    }
  }
}

void DeviceInAudio::on_hotplug_event(const JackPortEvent &ev) {
  // Only care about audio output ports (sources)
  if (ev.port_type != JACK_DEFAULT_AUDIO_TYPE) {
    return;
  }
  if (!(ev.flags & JackPortIsOutput)) {
    return;
  }

  pending_hotplug_.push_back(ev);

  // Schedule a one-shot tick to process them
  TickCb cb;
  cb.native = [this]() { this->process_hotplug_events(); };
  cb.oneshot = true;

  TickScheduler::instance().schedule(4, cb);
}

void DeviceInAudio::process_hotplug_events() {
  auto &jack = DeviceBackendJack::instance();

  // For each InputDecl
  for (auto &[id, decl] : input_decls_) {
    if (decl.type != "audio") {
      continue;
    }

    // Skip virtual ports (decl.name empty)
    if (decl.name.empty()) {
      continue;
    }

    // Get our internal JACK port
    auto it = input_ports_.find(id);
    if (it == input_ports_.end()) {
      continue;
    }

    jack_port_t *internal = it->second;
    std::string dst = jack.port_name(internal);

    // For each pending event
    for (auto &ev : pending_hotplug_) {
      if (ev.type == "add") {
        // Does this external port match the user pattern?
        if (match_string(decl.name, ev.full_name)) {
          // Connect external -> internal
          if (jack.connect(ev.full_name, dst)) {
            std::fprintf(
                stderr,
                "AUDIO hotplug: connected '%s' -> '%s'\n",
                ev.full_name.c_str(),
                dst.c_str()
            );
          }
        }
      }
    }
  }

  pending_hotplug_.clear();
}

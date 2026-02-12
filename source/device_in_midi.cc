#include "device_in_midi.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include <jack/ringbuffer.h>

#include "device_backend_jack.h"
#include "device_helpers.h"
#include "tick_scheduler.h"
#include "utils/time.h"

DeviceInMidi::~DeviceInMidi() {
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

bool DeviceInMidi::on_init() {
  if (ring_) {
    return true;
  }

  ring_ = jack_ringbuffer_create(MIDI_RINGBUFFER_BYTES);
  if (!ring_) {
    std::fprintf(stderr, "MIDI: failed to create ringbuffer\n");
    return false;
  }

  auto &jack = DeviceBackendJack::instance();
  if (!rt_registered_) {
    jack.add_rt_callback([this](jack_nframes_t nframes) { this->process(nframes); });
    rt_registered_ = true;
  }

  return true;
}

bool DeviceInMidi::match(InputDecl &decl, std::string &devnode_out) {
  if (!lazy_init()) {
    return false;
  }

  if (decl.type != "midi") {
    return false;
  }

  // unused, always create port
  devnode_out = "jack:midi:" + decl.id;
  return true;
}

bool DeviceInMidi::attach(const std::string &devnode, InputDecl &decl) {
  if (!lazy_init()) {
    return false;
  }

  // No sanitization — use exactly what user provided
  std::string port_name = decl.port.empty() ? decl.id : decl.port;

  auto &jack = DeviceBackendJack::instance();
  jack_port_t *in = jack.create_port(port_name, JACK_DEFAULT_MIDI_TYPE, JackPortIsInput);
  if (!in) {
    std::fprintf(stderr, "MIDI: failed to register input port '%s'\n", port_name.c_str());
    return false;
  }

  if (!decl.name.empty()) {
    std::string dst = jack.port_name(in);

    auto ports = jack.list_ports(JACK_DEFAULT_MIDI_TYPE, JackPortIsOutput);
    for (auto &full : ports) {
      if (match_string(decl.name, full)) {
        if (!jack.connect(full, dst)) {
          std::fprintf(
              stderr, "MIDI: failed to connect '%s' -> '%s'\n", full.c_str(), dst.c_str()
          );
        } else {
          std::fprintf(stderr, "MIDI: connected '%s' -> '%s'\n", full.c_str(), dst.c_str());
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
      std::fprintf(stderr, "MIDI: failed to schedule tick\n");
    }
  }

  return true;
}

bool DeviceInMidi::detach(const std::string &id) {
  if (!lazy_init()) {
    return false;
  }

  auto it = input_ports_.find(id);
  if (it == input_ports_.end()) {
    return false;
  }

  jack_port_t *in = it->second;

  auto &jack = DeviceBackendJack::instance();
  jack.destroy_port(in);
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

void DeviceInMidi::process(jack_nframes_t nframes) {
  if (!ring_) {
    return;
  }

  auto &jack = DeviceBackendJack::instance();

  for (auto &[id, port] : input_ports_) {
    void *buf = jack.port_buffer(port, nframes);
    uint32_t count = jack.midi_event_count(buf);

    for (uint32_t i = 0; i < count; i++) {
      jack_midi_event_t ev;
      if (!jack.midi_event_get(ev, buf, i)) {
        continue;
      }

      MidiEvent me;
      me.id = id;
      me.data.assign(ev.buffer, ev.buffer + ev.size);
      me.timestamp_us = AelkeyUtil::now("us");

      push_event(me);
    }
  }
}

void DeviceInMidi::push_event(const MidiEvent &ev) {
  if (!ring_) {
    return;
  }

  uint32_t size = static_cast<uint32_t>(ev.data.size());
  uint32_t id_len = static_cast<uint32_t>(ev.id.size());
  uint64_t timestamp_us = ev.timestamp_us;

  // size + id_len + id + timestamp + data
  size_t total = sizeof(size) + sizeof(id_len) + id_len + sizeof(timestamp_us) + size;

  if (jack_ringbuffer_write_space(ring_) < total) {
    return;
  }

  jack_ringbuffer_write(ring_, reinterpret_cast<const char *>(&size), sizeof(size));
  jack_ringbuffer_write(ring_, reinterpret_cast<const char *>(&id_len), sizeof(id_len));

  if (id_len) {
    jack_ringbuffer_write(ring_, ev.id.data(), id_len);
  }

  jack_ringbuffer_write(
      ring_, reinterpret_cast<const char *>(&timestamp_us), sizeof(timestamp_us)
  );

  if (size) {
    jack_ringbuffer_write(ring_, reinterpret_cast<const char *>(ev.data.data()), size);
  }
}

bool DeviceInMidi::pop_event(MidiEvent &out) {
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

  uint32_t size = header[0];
  uint32_t id_len = header[1];

  // Check if full message is available
  size_t total_needed = sizeof(uint32_t) + sizeof(uint32_t) + id_len + sizeof(uint64_t) + size;

  if (jack_ringbuffer_read_space(ring_) < total_needed) {
    return false;
  }

  // Safe to read
  jack_ringbuffer_read(ring_, reinterpret_cast<char *>(&size), sizeof(size));
  jack_ringbuffer_read(ring_, reinterpret_cast<char *>(&id_len), sizeof(id_len));

  out.id.resize(id_len);
  if (id_len) {
    jack_ringbuffer_read(ring_, out.id.data(), id_len);
  }

  jack_ringbuffer_read(ring_, reinterpret_cast<char *>(&out.timestamp_us), sizeof(uint64_t));

  out.data.resize(size);
  if (size) {
    jack_ringbuffer_read(ring_, reinterpret_cast<char *>(out.data.data()), size);
  }

  return true;
}

void DeviceInMidi::dispatch_batch_to_lua(
    const std::string &callback_name,
    const std::vector<MidiEvent> &events
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
    e["status"] = "ok";

    list[idx++] = e;
  }

  sol::protected_function pf = cb;
  sol::protected_function_result res = pf(list);

  if (!res.valid()) {
    sol::error err = res;
    std::fprintf(stderr, "Lua MIDI batch callback error: %s\n", err.what());
  }
}

void DeviceInMidi::pump_messages() {
  MidiEvent ev;
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

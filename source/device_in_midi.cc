#include "device_in_midi.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include <jack/ringbuffer.h>

#include "aelkey_util.h"
#include "device_backend_jack.h"
#include "device_helpers.h"
#include "tick_scheduler.h"

static constexpr size_t MIDI_RINGBUFFER_BYTES = 64 * 1024;

DeviceInMidi::~DeviceInMidi() {
  if (tick_fd_ >= 0) {
    TickScheduler::instance().unregister_fd(tick_fd_);
    tick_fd_ = -1;
  }

  auto &jack = DeviceBackendJack::instance();
  for (auto &kv : inputs_) {
    jack.destroy_port(kv.second);
  }

  inputs_.clear();
  source_ports_.clear();

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

  if (decl.name.empty()) {
    devnode_out = "jack:midi:" + decl.id;
    return true;
  }

  auto &jack = DeviceBackendJack::instance();
  auto ports = jack.list_ports(JACK_DEFAULT_MIDI_TYPE, JackPortIsOutput);

  std::string result;

  for (auto &full : ports) {
    if (match_string(decl.name, full)) {
      result = full;
      break;
    }
  }

  if (result.empty()) {
    return false;
  }

  devnode_out = "jack:midi:" + result;
  return true;
}

bool DeviceInMidi::attach(const std::string &devnode, InputDecl &decl) {
  if (!lazy_init()) {
    return false;
  }

  if (devnode.rfind("jack:midi:", 0) != 0) {
    std::fprintf(stderr, "MIDI: invalid devnode '%s'\n", devnode.c_str());
    return false;
  }

  std::string src = devnode.substr(std::strlen("jack:midi:"));  // "Client:Port"

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
    if (!jack.connect(src, dst)) {
      std::fprintf(stderr, "MIDI: failed to connect '%s' -> '%s'\n", src.c_str(), dst.c_str());
      jack.destroy_port(in);
      return false;
    }
  }

  inputs_[decl.id] = in;
  source_ports_[in] = src;

  decl.devnode = devnode;
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

  auto it = inputs_.find(id);
  if (it == inputs_.end()) {
    return false;
  }

  jack_port_t *in = it->second;
  source_ports_.erase(in);

  auto &jack = DeviceBackendJack::instance();
  jack.destroy_port(in);
  inputs_.erase(it);

  if (inputs_.empty() && tick_fd_ >= 0) {
    TickScheduler::instance().unregister_fd(tick_fd_);
    tick_fd_ = -1;
  }

  return true;
}

void DeviceInMidi::process(jack_nframes_t nframes) {
  if (!ring_) {
    return;
  }

  auto &jack = DeviceBackendJack::instance();

  for (auto &[id, port] : inputs_) {
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
      me.timestamp_us = util_now("us");

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

  size_t total = sizeof(size) + sizeof(id_len) + id_len + size;
  if (jack_ringbuffer_write_space(ring_) < total) {
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

bool DeviceInMidi::pop_event(MidiEvent &out) {
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

void DeviceInMidi::dispatch_batch_to_lua(
    const std::string &id,
    const std::vector<MidiEvent> &events
) {
  auto &state = AelkeyState::instance();
  sol::state_view lua(state.lua_vm);

  auto it = state.input_map.find(id);
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

  sol::table list = lua.create_table();
  int idx = 1;

  for (auto &ev : events) {
    sol::table e = lua.create_table();

    e["device"] = id;
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
    batches_[ev.id].events.push_back(ev);
  }

  for (auto &[id, batch] : batches_) {
    if (!batch.events.empty()) {
      dispatch_batch_to_lua(id, batch.events);
      batch.events.clear();
    }
  }
}

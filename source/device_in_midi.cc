#include "device_in_midi.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "backend_jack.h"
#include "dispatcher.h"
#include "dispatcher_event.h"
#include "dispatcher_timer.h"
#include "utils/regex_match.h"
#include "utils/signal.h"
#include "utils/time.h"

DeviceInMidi::~DeviceInMidi() {
  auto &jack = BackendJack::instance();
  for (auto &kv : input_ports_) {
    jack.destroy_port(kv.second);
  }

  input_ports_.clear();
  input_decls_.clear();
}

bool DeviceInMidi::on_init() {
  auto &jack = BackendJack::instance();
  tok_jack_process_ =
      jack.sig_jack_process_.subscribe([this](jack_nframes_t nframes) { process(nframes); });

  tok_jack_hotplug_ = jack.sig_jack_hotplug_.subscribe([this](const JackPortEvent &ev) {
    on_hotplug_event(ev);
  });

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

  auto &jack = BackendJack::instance();
  jack_port_t *in = jack.create_port(port_name, JACK_DEFAULT_MIDI_TYPE, JackPortIsInput);
  if (!in) {
    std::fprintf(stderr, "MIDI: failed to register input port '%s'\n", port_name.c_str());
    return false;
  }

  if (!decl.name.empty()) {
    std::string dst = jack.port_name(in);

    auto ports = jack.list_ports(JACK_DEFAULT_MIDI_TYPE, JackPortIsOutput);
    for (auto &full : ports) {
      if (AelkeyUtil::match_string(decl.name, full)) {
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

  if (dispatch_fd_ < 0) {
    DispatcherCb cb;
    cb.native = [this]() { this->pump_messages(); };
    cb.oneshot = false;

    dispatch_fd_ = DispatcherEvent::instance().create(cb);
    if (dispatch_fd_ < 0) {
      std::fprintf(stderr, "MIDI: failed to create event dispatcher\n");
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

  auto &jack = BackendJack::instance();
  jack.destroy_port(in);
  input_ports_.erase(it);

  if (input_ports_.empty() && dispatch_fd_ >= 0) {
    DispatcherEvent::instance().unregister_fd(dispatch_fd_);
    dispatch_fd_ = -1;
  }

  auto it2 = input_decls_.find(id);
  if (it2 != input_decls_.end()) {
    input_decls_.erase(it2);
  }

  return true;
}

void DeviceInMidi::process(jack_nframes_t nframes) {
  auto &jack = BackendJack::instance();

  bool queued = false;

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

      queue_.enqueue(me);
      queued = true;
    }
  }

  if (queued && dispatch_fd_ >= 0) {
    DispatcherEvent::instance().trigger(dispatch_fd_);
  }
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
  while (queue_.try_dequeue(ev)) {
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

void DeviceInMidi::on_hotplug_event(const JackPortEvent &ev) {
  // Only care about MIDI output ports (sources)
  if (ev.port_type != JACK_DEFAULT_MIDI_TYPE) {
    return;
  }
  if (!(ev.flags & JackPortIsOutput)) {
    return;
  }

  pending_hotplug_.push_back(ev);

  // Schedule a one-shot timer to process them
  DispatcherCb cb;
  cb.native = [this]() { this->process_hotplug_events(); };
  cb.oneshot = true;

  DispatcherTimer::instance().schedule(4, cb);
}

void DeviceInMidi::process_hotplug_events() {
  auto &jack = BackendJack::instance();

  // For each InputDecl
  for (auto &[id, decl] : input_decls_) {
    if (decl.type != "midi") {
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
        if (AelkeyUtil::match_string(decl.name, ev.full_name)) {
          // Connect external -> internal
          if (jack.connect(ev.full_name, dst)) {
            std::fprintf(
                stderr,
                "MIDI hotplug: connected '%s' -> '%s'\n",
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

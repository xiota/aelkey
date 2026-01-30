#pragma once

#include <map>
#include <string>
#include <vector>

#include <jack/jack.h>
#include <jack/midiport.h>
#include <jack/ringbuffer.h>

#include "aelkey_state.h"
#include "device_backend.h"
#include "device_declarations.h"
#include "singleton.h"

struct MidiEvent {
  std::string id;             // InputDecl id
  std::vector<uint8_t> data;  // raw MIDI bytes
};

class DeviceBackendMidi : public DeviceBackend, public Singleton<DeviceBackendMidi> {
  friend class Singleton<DeviceBackendMidi>;

 protected:
  DeviceBackendMidi() = default;
  ~DeviceBackendMidi();

  bool on_init() override;

 public:
  bool match(const InputDecl &decl, std::string &devnode_out) override;
  bool attach(const std::string &devnode, InputDecl &decl) override;
  bool detach(const std::string &id) override;

  void pump_messages();

 private:
  static int process_cb(jack_nframes_t nframes, void *arg);
  void process(jack_nframes_t nframes);

  void push_event(const MidiEvent &ev);
  bool pop_event(MidiEvent &out);

  void dispatch_to_lua(const MidiEvent &ev);

  std::string ensure_client_name(const InputDecl &decl);
  std::string make_default_port_name(const InputDecl &decl) const;
  std::string sanitize_port_name(const std::string &name) const;

 private:
  jack_client_t *client_ = nullptr;
  jack_ringbuffer_t *ring_ = nullptr;

  // id -> JACK port
  std::map<std::string, jack_port_t *> inputs_;

  // JACK port -> source "Client:Port" string
  std::map<jack_port_t *, std::string> source_ports_;

  // client name actually used
  std::string client_name_;

  int tick_fd_ = -1;
};

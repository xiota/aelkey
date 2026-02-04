#pragma once

#include <map>
#include <string>
#include <vector>

#include <jack/ringbuffer.h>

#include "aelkey_state.h"
#include "device_backend_jack.h"
#include "device_declarations.h"
#include "device_in.h"
#include "singleton.h"

struct MidiEvent {
  std::string id;             // InputDecl id
  std::vector<uint8_t> data;  // raw MIDI bytes
  uint64_t timestamp_us;
};

struct MidiBatch {
  std::vector<MidiEvent> events;
};

class DeviceInMidi : public DeviceIn, public Singleton<DeviceInMidi> {
  friend class Singleton<DeviceInMidi>;

 protected:
  DeviceInMidi() = default;
  ~DeviceInMidi();

  bool on_init() override;

 public:
  bool match(InputDecl &decl, std::string &devnode_out) override;
  bool attach(const std::string &devnode, InputDecl &decl) override;
  bool detach(const std::string &id) override;

  void pump_messages();

 private:
  void process(jack_nframes_t nframes);

  void push_event(const MidiEvent &ev);
  bool pop_event(MidiEvent &out);

  void dispatch_batch_to_lua(const std::string &id, const std::vector<MidiEvent> &events);

 private:
  jack_ringbuffer_t *ring_ = nullptr;

  // id -> JACK port
  std::map<std::string, jack_port_t *> inputs_;

  // JACK port -> source "Client:Port" string
  std::map<jack_port_t *, std::string> source_ports_;

  int tick_fd_ = -1;
  std::map<std::string, MidiBatch> batches_;

  bool rt_registered_ = false;
};

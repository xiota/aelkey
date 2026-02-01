#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include <jack/jack.h>
#include <jack/midiport.h>
#include <jack/ringbuffer.h>

#include "device_declarations.h"  // OutputDecl
#include "device_helpers.h"       // match_string
#include "device_out.h"
#include "singleton.h"

class DeviceOutMidi : public DeviceOut, public Singleton<DeviceOutMidi> {
  friend class Singleton<DeviceOutMidi>;

 protected:
  DeviceOutMidi() = default;
  ~DeviceOutMidi();

  bool on_init() override;

 public:
  bool create(const OutputDecl &decl) override;

  bool send(const std::string &id, const uint8_t *data, size_t len);

  bool send(const std::string &id, const std::vector<uint8_t> &msg) {
    return send(id, msg.data(), msg.size());
  }

  bool destroy(const std::string &id);

 private:
  bool ensure_client_name(const OutputDecl &decl);
  static int process_cb(jack_nframes_t nframes, void *arg);
  void process(jack_nframes_t nframes);

  jack_client_t *client_ = nullptr;
  std::string client_name_;

  // id -> JACK port
  std::map<std::string, jack_port_t *> outputs_;

  // Single ringbuffer for outgoing events.
  jack_ringbuffer_t *ring_ = nullptr;
  static constexpr size_t kRingSize = 4096;  // symmetric with input
};

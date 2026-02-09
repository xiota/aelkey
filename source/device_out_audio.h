#pragma once

#include <cstdint>
#include <map>
#include <string>

#include <jack/ringbuffer.h>

#include "device_backend_jack.h"
#include "device_declarations.h"
#include "device_helpers.h"
#include "device_out.h"
#include "singleton.h"

class DeviceOutAudio : public DeviceOut, public Singleton<DeviceOutAudio> {
  friend class Singleton<DeviceOutAudio>;

 protected:
  DeviceOutAudio() = default;
  ~DeviceOutAudio();

  bool on_init() override;

 public:
  bool create(const OutputDecl &decl) override;

  // Send 'frames' samples of mono audio for the given output id.
  // Samples are expected to be float32, non-interleaved, mono.
  bool send(const std::string &id, const float *samples, size_t frames);

  bool destroy(const std::string &id);

 private:
  void process(jack_nframes_t nframes);

 private:
  // id -> JACK port
  std::map<std::string, jack_port_t *> outputs_;

  jack_ringbuffer_t *ring_ = nullptr;
  static constexpr size_t kRingSize = 512 * 1024;

  bool rt_registered_ = false;
};

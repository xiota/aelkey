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

  void on_hotplug_event(const JackPortEvent &ev);
  void process_hotplug_events();

 private:
  // key = id
  std::map<std::string, jack_port_t *> output_ports_;
  std::map<std::string, OutputDecl> output_decls_;

  jack_ringbuffer_t *ring_ = nullptr;
  static constexpr size_t kRingSize = 512 * 1024;

  RtCallback rt_cb_;
  bool rt_registered_ = false;

  std::vector<JackPortEvent> pending_hotplug_;
  HotplugCallback hp_cb_;
  bool hotplug_registered_ = false;
};

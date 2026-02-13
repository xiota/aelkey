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

struct AudioEvent {
  std::string id;             // InputDecl id
  std::vector<uint8_t> data;  // raw float32 bytes
  uint64_t timestamp_us = 0;
  uint32_t frames = 0;
};

struct AudioBatch {
  std::vector<AudioEvent> events;
};

class DeviceInAudio : public DeviceIn, public Singleton<DeviceInAudio> {
  friend class Singleton<DeviceInAudio>;

 protected:
  DeviceInAudio() = default;
  ~DeviceInAudio();

  bool on_init() override;

 public:
  bool match(InputDecl &decl, std::string &devnode_out) override;
  bool attach(const std::string &devnode, InputDecl &decl) override;
  bool detach(const std::string &id) override;

  void pump_messages();

 private:
  void process(jack_nframes_t nframes);

  void push_event(const AudioEvent &ev);
  bool pop_event(AudioEvent &out);

  void dispatch_batch_to_lua(
      const std::string &callback_name,
      const std::vector<AudioEvent> &events
  );

  void on_hotplug_event(const JackPortEvent &ev);
  void process_hotplug_events();

 private:
  jack_ringbuffer_t *ring_ = nullptr;

  // key = id
  std::map<std::string, jack_port_t *> input_ports_;
  std::map<std::string, InputDecl> input_decls_;

  int tick_fd_ = -1;

  // key = callback name
  std::map<std::string, std::vector<AudioEvent>> batches_;
  RtCallback rt_cb_;
  bool rt_registered_ = false;

  std::vector<JackPortEvent> pending_hotplug_;
  HotplugCallback hp_cb_;
  bool hotplug_registered_ = false;

  static constexpr size_t AUDIO_RINGBUFFER_BYTES = 512 * 1024;
};

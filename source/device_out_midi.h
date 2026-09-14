#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include <readerwriterqueue.h>

#include "backend_jack.h"
#include "device_declarations.h"
#include "device_out.h"
#include "singleton.h"
#include "utils/signal.h"

class DeviceOutMidi : public DeviceOut, public Singleton<DeviceOutMidi> {
  friend class Singleton<DeviceOutMidi>;

 protected:
  DeviceOutMidi() = default;
  ~DeviceOutMidi() = default;

  bool on_init() override;

 public:
  void shutdown();

  bool create(const OutputDecl &decl) override;

  bool send(const std::string &id, const uint8_t *data, size_t len);

  bool send(const std::string &id, const std::vector<uint8_t> &msg) {
    return send(id, msg.data(), msg.size());
  }

  bool destroy(const std::string &id);

 private:
  void process(jack_nframes_t nframes);

  void on_hotplug_event(const JackPortEvent &ev);
  void process_hotplug_events();

 private:
  moodycamel::ReaderWriterQueue<MidiEvent> queue_;

  // key = id
  std::map<std::string, JackPortRAII> output_ports_;
  std::map<std::string, OutputDecl> output_decls_;

  std::vector<JackPortEvent> pending_hotplug_;

  // tokens
  AelkeyUtil::Signal<void(const JackPortEvent &)>::Connection tok_jack_hotplug_;
  AelkeyUtil::Signal<void(jack_nframes_t)>::Connection tok_jack_process_;
  AelkeyUtil::Signal<void(void)>::Connection tok_jack_shutdown_;
};

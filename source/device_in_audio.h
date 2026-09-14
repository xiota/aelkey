#pragma once

#include <map>
#include <string>
#include <vector>

#include <readerwriterqueue.h>

#include "aelkey_state.h"
#include "backend_jack.h"
#include "device_declarations.h"
#include "device_in.h"
#include "singleton.h"
#include "utils/signal.h"

class DeviceInAudio : public DeviceIn, public Singleton<DeviceInAudio> {
  friend class Singleton<DeviceInAudio>;

 protected:
  DeviceInAudio() = default;
  ~DeviceInAudio() = default;

  bool on_init() override;

 public:
  void shutdown();

  bool match(InputDecl &decl, std::string &devnode_out) override;
  bool attach(const std::string &devnode, InputDecl &decl) override;
  bool detach(const std::string &id) override;

  void pump_messages();

 private:
  void process(jack_nframes_t nframes);

  void dispatch_batch_to_lua(
      const std::string &callback_name,
      const std::vector<AudioEvent> &events
  );

  void on_hotplug_event(const JackPortEvent &ev);
  void process_hotplug_events();

 private:
  moodycamel::ReaderWriterQueue<AudioEvent> queue_;

  // key = id
  std::map<std::string, JackPortRAII> input_ports_;
  std::map<std::string, InputDecl> input_decls_;

  int dispatch_fd_ = -1;

  // key = callback name
  std::map<std::string, std::vector<AudioEvent>> batches_;

  std::vector<JackPortEvent> pending_hotplug_;

  // tokens
  AelkeyUtil::Signal<void(const JackPortEvent &)>::Connection tok_jack_hotplug_;
  AelkeyUtil::Signal<void(jack_nframes_t)>::Connection tok_jack_process_;
  AelkeyUtil::Signal<void(void)>::Connection tok_jack_shutdown_;
};

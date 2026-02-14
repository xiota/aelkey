#pragma once

#include "device_declarations.h"
#include "singleton.h"
#include "utils/signal.h"

class RouterDeviceState : public Singleton<RouterDeviceState> {
  friend class Singleton<RouterDeviceState>;

 protected:
  RouterDeviceState();
  ~RouterDeviceState() = default;

 public:
  void notify_state_change(const InputDecl &decl, const char *state);

 public:
  AelkeyUtil::Signal<void(const InputDecl &decl, const char *state)>::Connection
      tok_state_changed_;
};

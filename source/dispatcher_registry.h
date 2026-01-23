#pragma once

#include <vector>

#include <sys/epoll.h>

#include "aelkey_state.h"
#include "dispatcher.h"

// Registry of all dispatcher singletons
inline std::vector<DispatcherBase *> &dispatcher_registry() {
  static std::vector<DispatcherBase *> registry;
  return registry;
}

// Auto-registration helper, identical to TweakUiRegistry<T>
template <typename T>
struct DispatcherRegistry {
  DispatcherRegistry() {
    dispatcher_registry().push_back(&T::instance());
  }
};

inline void init_all_dispatchers() {
  for (DispatcherBase *d : dispatcher_registry()) {
    d->init();
  }
}

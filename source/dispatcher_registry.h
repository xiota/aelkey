#pragma once

#include <map>
#include <string>

#include <sys/epoll.h>

#include "aelkey_state.h"
#include "dispatcher.h"

// Registry of all dispatcher singletons
inline auto &dispatcher_registry() {
  static std::map<std::string, DispatcherBase *> registry;
  return registry;
}

// Auto-registration helper
template <typename T>
struct DispatcherRegistry {
  DispatcherRegistry() {
    auto &registry = dispatcher_registry();
    registry[T::instance().type()] = &T::instance();
  }
};

inline void init_all_dispatchers() {
  for (auto &[type, dispatcher] : dispatcher_registry()) {
    dispatcher->init();
  }
}

inline DispatcherBase *get_dispatcher_for_type(const std::string &type) {
  auto &registry = dispatcher_registry();
  auto it = registry.find(type);
  return (it != registry.end()) ? it->second : nullptr;
}

inline void init_dispatcher_for_type(const std::string &type) {
  if (auto *d = get_dispatcher_for_type(type)) {
    d->init();
  }
}

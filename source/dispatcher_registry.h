#pragma once

#include <map>
#include <string>

class DispatcherBase;

inline auto &dispatcher_registry() {
  static std::map<std::string, DispatcherBase *> registry;
  return registry;
}

inline DispatcherBase *get_dispatcher_for_type(const std::string &type) {
  auto &registry = dispatcher_registry();
  auto it = registry.find(type);
  return (it != registry.end()) ? it->second : nullptr;
}

bool init_dispatcher_for_type(const std::string &type);

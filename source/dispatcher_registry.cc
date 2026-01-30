#include "dispatcher_registry.h"

#include "dispatcher.h"

bool init_dispatcher_for_type(const std::string &type) {
  if (auto *d = get_dispatcher_for_type(type)) {
    return d->lazy_init();
  }
  return false;
}

#pragma once

#include <functional>
#include <string>

#include <libudev.h>

#include "dispatcher.h"
#include "dispatcher_registry.h"
#include "singleton.h"

struct InputDecl;

class DispatcherUdev : public Dispatcher<DispatcherUdev> {
  friend class Singleton<DispatcherUdev>;

 protected:
  DispatcherUdev() = default;
  ~DispatcherUdev();

 public:
  const char *type() const override;

  void ensure_initialized();
  void handle_event(EpollPayload *, uint32_t events) override;

  std::string enumerate_and_match(
      const char *subsystem,
      const std::function<std::string(struct udev_device *)> &matcher
  );

  struct udev *get_udev() const;

  void notify_state_change(const InputDecl &decl, const char *state);

 private:
  void handle_udev_add(struct udev_device *dev);
  void handle_udev_remove(struct udev_device *dev);

 private:
  struct udev *udev_ctx_ = nullptr;
  struct udev_monitor *mon_ = nullptr;
  int mon_fd_ = -1;
};

template class Dispatcher<DispatcherUdev>;

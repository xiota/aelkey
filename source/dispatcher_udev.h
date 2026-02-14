#pragma once

#include <functional>
#include <string>

#include <libudev.h>

#include "dispatcher.h"
#include "singleton.h"
#include "utils/signal.h"

struct InputDecl;

struct UdevEvent {
  std::string action;
  std::string subsystem;
  std::string devnode;
  std::string syspath;

  // Optional USB metadata
  std::string devtype;
  std::string vid;
  std::string pid;
  std::string busnum;
  std::string devnum;
};

class DispatcherUdev : public Dispatcher<DispatcherUdev> {
  friend class Singleton<DispatcherUdev>;
  friend class Dispatcher<DispatcherUdev>;

 protected:
  DispatcherUdev() = default;
  ~DispatcherUdev();

  bool on_init() override;

  bool auto_init_ = true;

 public:
  const char *type() const override;

  void handle_event(EpollPayload *, uint32_t events) override;

  std::string enumerate_and_match(
      const char *subsystem,
      const std::function<std::string(struct udev_device *)> &matcher
  );

  struct udev *get_udev() const;

 public:
  AelkeyUtil::Signal<void(const UdevEvent &)> sig_udev_event_;

 private:
  void handle_udev_add(struct udev_device *dev);
  void handle_udev_remove(struct udev_device *dev);

 private:
  struct udev *udev_ctx_ = nullptr;
  struct udev_monitor *mon_ = nullptr;
  int mon_fd_ = -1;
};

template class Dispatcher<DispatcherUdev>;

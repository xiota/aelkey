#pragma once

#include <map>
#include <string>
#include <sys/epoll.h>
#include <unistd.h>
#include <vector>

#include "dispatcher_next.h"
#include "singleton.h"

class DispatcherVulgate : public DispatcherNext<DispatcherVulgate> {
  friend class Singleton<DispatcherVulgate>;
  friend class DispatcherNext<DispatcherVulgate>;

 protected:
  DispatcherVulgate() = default;
  ~DispatcherVulgate() override {
    cleanup_fds();
  }

 public:
  const char *type() const override {
    return "vulgate";
  }

  void cleanup_fds() override {
    device_ids_.clear();
    DispatcherNextBase::cleanup_fds();
  }

  // Generic registration for any file descriptor and callback
  void
  register_device_fd(int fd, uint32_t events, DispatcherCb cb, std::string device_id = "") {
    if (!device_id.empty()) {
      device_ids_[fd] = std::move(device_id);
    }
    set_callback(fd, std::move(cb));
    register_fd(fd, events);
  }

  void unregister_device_fd(int fd) {
    device_ids_.erase(fd);
    clear_callback(fd);
    unregister_fd(fd);
  }

 private:
  std::map<int, std::string> device_ids_;
};

template class DispatcherNext<DispatcherVulgate>;

#pragma once

#include <functional>
#include <map>
#include <optional>
#include <string>

#include "device_declarations.h"
#include "device_in.h"
#include "device_out.h"
#include "singleton.h"
#include "utils/signal.h"

class ManagerDevice : public Singleton<ManagerDevice> {
  friend class Singleton<ManagerDevice>;

 protected:
  ManagerDevice();
  ~ManagerDevice() = default;

 public:
  DeviceIn *input_backend(const std::string &type);
  DeviceOut *output_backend(const std::string &type);

  bool match_input(InputDecl &decl, std::string &devnode_out);
  bool attach_input(const std::string &devnode, InputDecl &decl);
  std::optional<InputDecl> detach_output(const std::string &dev_id);

  bool create_output(const OutputDecl &decl);

 public:
  AelkeyUtil::Signal<void(const InputDecl &decl, const char *state)> sig_state_changed_;

 private:
  template <typename T>
    requires std::derived_from<T, DeviceIn>
  void register_input(std::string type) {
    input_getters_[std::move(type)] = []() -> DeviceIn * { return &T::instance(); };
  }

  template <typename T>
    requires std::derived_from<T, DeviceOut>
  void register_output(std::string type) {
    output_getters_[std::move(type)] = []() -> DeviceOut * { return &T::instance(); };
  }

 private:
  std::map<std::string, DeviceIn *> cached_inputs_;
  std::map<std::string, DeviceOut *> cached_outputs_;

  std::map<std::string, std::function<DeviceIn *()>> input_getters_;
  std::map<std::string, std::function<DeviceOut *()>> output_getters_;
};

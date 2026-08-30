#pragma once

#include <map>
#include <string>

#include "device_declarations.h"
#include "device_out.h"
#include "singleton.h"

class DeviceOutUhid : public DeviceOut, public Singleton<DeviceOutUhid> {
  friend class Singleton<DeviceOutUhid>;

 public:
  bool create(const OutputDecl &decl) override;

  int get_fd(const std::string &id) const;

  void write_report(const std::string &id, const std::string &type, const std::string &data);
  void
  reply(const std::string &id, uint32_t trans_id, const int status, const std::string &data);

 private:
  DeviceOutUhid() = default;
  ~DeviceOutUhid();

  struct DeviceContext {
    int fd = -1;
    std::string on_report;
  };

  std::map<std::string, DeviceContext> devices_;
};

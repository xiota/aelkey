#pragma once

#include <map>
#include <string>
#include <unordered_set>
#include <vector>

#include "backend_udev.h"
#include "device_declarations.h"
#include "singleton.h"
#include "utils/lua_helpers.h"
#include "utils/signal.h"

struct WatchListPayload {
  std::string ref;
  std::string id;
  std::string type;
  std::string state;

  sol::table to_lua(sol::state_view lua) const {
    sol::table t = lua.create_table();
    AelkeyUtil::lua_set_field(t, "ref", ref);
    AelkeyUtil::lua_set_field(t, "id", id);
    AelkeyUtil::lua_set_field(t, "type", type);
    AelkeyUtil::lua_set_field(t, "state", state);
    return t;
  }
};

class RouterWatchList : public Singleton<RouterWatchList> {
  friend class Singleton<RouterWatchList>;

 protected:
  RouterWatchList();
  ~RouterWatchList() = default;

 public:
  void add_watch(const std::string &entry_id, const InputDecl &decl);
  void erase_watch(const std::string &entry_id);

  void notify_watch(const std::string &entry_id, const InputDecl &decl, const char *state);

  std::vector<std::string> keys() const;

  void set_callback(const std::string &cb);

  void enumerate_now(std::string devnode = "");

 private:
  AelkeyUtil::Signal<void(const UdevEvent &)>::Connection tok_udev_event_;

  // key: entry_id
  std::map<std::string, std::vector<InputDecl>> watch_map_;

  // key: devnode
  struct ActiveEntry {
    std::string entry_id;
    InputDecl decl;
  };
  std::unordered_map<std::string, ActiveEntry> active_map_;

  std::string on_watchlist_;
};

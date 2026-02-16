#include "router_watch_list.h"

#include <string>
#include <vector>

#include <sol/sol.hpp>

#include "aelkey_state.h"
#include "device_declarations.h"
#include "dispatcher_udev.h"
#include "manager_device_in.h"

RouterWatchList::RouterWatchList() {
  tok_udev_event_ =
      DispatcherUdev::instance().sig_udev_event_.subscribe([this](const UdevEvent &ev) {
        if (ev.action == "remove") {
          auto it = active_map_.find(ev.devnode);
          if (it != active_map_.end()) {
            notify_watch(it->second.entry_id, it->second.decl, "remove");
            active_map_.erase(it);
          }
        }

        if (ev.action == "add") {
          if (!active_map_.count(ev.devnode)) {
            enumerate_now(ev.devnode);
          }
        }
      });
}

void RouterWatchList::add_watch(const std::string &entry_id, const InputDecl &decl) {
  watch_map_[entry_id].push_back(decl);
}

void RouterWatchList::erase_watch(const std::string &entry_id) {
  watch_map_.erase(entry_id);
}

void RouterWatchList::notify_watch(
    const std::string &entry_id,
    const InputDecl &decl,
    const char *state
) {
  if (on_watchlist_.empty()) {
    return;
  }

  sol::state_view lua(AelkeyState::instance().lua_vm);
  sol::object obj = lua[on_watchlist_];
  if (!obj.is<sol::function>()) {
    return;
  }

  sol::function cb = obj.as<sol::function>();

  sol::table tbl = lua.create_table();
  tbl["ref"] = entry_id;
  tbl["id"] = decl.id;
  tbl["type"] = decl.type;
  tbl["state"] = state ? state : "";

  sol::protected_function pf = cb;
  sol::protected_function_result result = pf(tbl);

  if (!result.valid()) {
    sol::error err = result;
    std::fprintf(stderr, "Lua watchlist_callback error: %s\n", err.what());
  }
}

std::vector<std::string> RouterWatchList::keys() const {
  std::vector<std::string> out;
  out.reserve(watch_map_.size());
  for (auto &kv : watch_map_) {
    out.push_back(kv.first);
  }
  return out;
}

void RouterWatchList::set_callback(const std::string &cb) {
  on_watchlist_ = cb;
}

void RouterWatchList::enumerate_now(std::string devnode) {
  for (auto &[entry_id, list] : watch_map_) {
    for (auto &decl : list) {
      std::string matched;
      if (ManagerDeviceIn::instance().match(decl, matched)) {
        if (!devnode.empty()) {
          if (matched == devnode) {
            active_map_[matched] = { entry_id, decl };
            notify_watch(entry_id, decl, "match");
            break;
          }
        } else if (!active_map_.count(matched)) {
          active_map_[matched] = { entry_id, decl };
          notify_watch(entry_id, decl, "match");
        }
      }
    }
  }
}

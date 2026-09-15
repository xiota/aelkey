#pragma once

#include <vector>

#include <sol/sol.hpp>

namespace AelkeyUtil {

// Lua to C++
template <typename T>
void lua_get_field(sol::table t, const char *name, T &member) {
  sol::object value = t[name];
  if (value.valid() && value.is<T>()) {
    member = value.as<T>();
  }
}

template <typename T>
void lua_get_field(sol::table t, int index, T &member) {
  sol::object value = t[index];
  if (value.valid() && value.is<T>()) {
    member = value.as<T>();
  }
}

template <typename T>
void lua_get_vector(sol::table t, const char *name, std::vector<T> &output) {
  sol::object value = t[name];
  if (!value.valid() || !value.is<sol::table>()) {
    return;
  }
  value.as<sol::table>().for_each([&](sol::object, sol::object item) {
    if (item.is<T>()) {
      output.push_back(item.as<T>());
    }
  });
}

// C++ to Lua
template <typename T>
void lua_set_field(sol::table t, const char *name, const T &value) {
  t[name] = value;
}

template <typename T>
void lua_set_field(sol::table t, const char *name, const std::optional<T> &value) {
  if (value) {
    t[name] = *value;
  }
}

template <typename T>
void lua_set_vector(sol::table t, const char *name, const std::vector<T> &values) {
  sol::table result = t.create_named(name);

  int index = 1;
  for (const auto &value : values) {
    result[index++] = value;
  }
}

}  // namespace AelkeyUtil

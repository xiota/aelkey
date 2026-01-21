#include "device_helpers.h"

#include <regex>
#include <string_view>

bool match_string(std::string_view pattern, std::string_view value) {
  if (pattern.empty()) {
    return true;
  }

  if (looks_like_regex(pattern)) {
    try {
      std::regex re{ std::string(pattern) };
      return std::regex_match(value.begin(), value.end(), re);
    } catch (...) {
      return pattern == value;
    }
  }

  return pattern == value;
}

bool looks_like_regex(std::string_view s) {
  if (s.empty()) {
    return false;
  }
  if (s.front() == '^' || s.back() == '$') {
    return true;
  }
  if (s.find(".*") != std::string_view::npos) {
    return true;
  }
  if (s.find(".+") != std::string_view::npos) {
    return true;
  }
  return false;
}

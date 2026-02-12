#pragma once

#include <cstdint>
#include <string_view>

#include <time.h>

namespace AelkeyUtil {

// now("ms"|"us"|"ns")
inline uint64_t now(std::string_view unit) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);

  if (unit == "us") {
    return ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000ULL;
  } else if (unit == "ns") {
    return ts.tv_sec * 1000000000ULL + ts.tv_nsec;
  } else {  // default ms
    return ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000ULL;
  }
}

}  // namespace AelkeyUtil

#pragma once

#include <string>
#include <unordered_map>
#include <utility>

#include <linux/input.h>

struct PairHash {
  std::size_t operator()(const std::pair<std::string, int> &p) const noexcept {
    std::size_t h1 = std::hash<std::string>{}(p.first);
    std::size_t h2 = std::hash<int>{}(p.second);
    return h1 ^ (h2 << 1);
  }
};

struct PairEq {
  bool operator()(
      const std::pair<std::string, int> &a,
      const std::pair<std::string, int> &b
  ) const noexcept {
    return a.first == b.first && a.second == b.second;
  }
};

struct HapticsSourceCtx {
  std::string id;  // "virt_gamepad"
  int fd = -1;     // uinput FD

  std::string callback;

  // virtual_id → ff_effect
  std::unordered_map<int, ff_effect> effects;
};

struct HapticsSinkCtx {
  std::string id;  // "gamepad"
  int fd = -1;     // evdev FD

  // key: (source_id, virt_id) → real_id
  std::unordered_map<std::pair<std::string, int>, int, PairHash, PairEq> slots;
};

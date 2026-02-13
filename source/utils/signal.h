#pragma once

#include <functional>
#include <list>
#include <type_traits>
#include <utility>
#include <vector>

namespace AelkeyUtil {

template <typename Signature>
class Signal;  // primary template

template <typename R, typename... Args>
class Signal<R(Args...)> {
 public:
  using Callback = std::function<R(Args...)>;
  using CallbackIt = typename std::list<Callback>::iterator;

  class Connection {
   public:
    Connection() = default;

    Connection(Signal *sig, CallbackIt it) : sig_(sig), it_(it) {}

    Connection(const Connection &) = delete;
    Connection &operator=(const Connection &) = delete;

    Connection(Connection &&other) noexcept : sig_(other.sig_), it_(other.it_) {
      other.sig_ = nullptr;
    }

    Connection &operator=(Connection &&other) noexcept {
      if (this != &other) {
        disconnect();
        sig_ = other.sig_;
        it_ = other.it_;
        other.sig_ = nullptr;
      }
      return *this;
    }

    ~Connection() {
      disconnect();
    }

    void disconnect() {
      if (sig_) {
        sig_->unsubscribe(*this);
        sig_ = nullptr;
      }
    }

    bool connected() const noexcept {
      return sig_ != nullptr;
    }

   private:
    friend class Signal;
    Signal *sig_ = nullptr;
    CallbackIt it_{};
  };

  Signal() = default;
  Signal(const Signal &) = delete;
  Signal &operator=(const Signal &) = delete;
  Signal(Signal &&) = default;
  Signal &operator=(Signal &&) = default;

  Connection subscribe(Callback cb) {
    auto it = callbacks_.insert(callbacks_.end(), std::move(cb));
    return Connection(this, it);
  }

  void unsubscribe(Connection &c) {
    if (!c.sig_ || c.sig_ != this) {
      return;
    }
    callbacks_.erase(c.it_);
    c.sig_ = nullptr;
  }

  std::conditional_t<std::is_void_v<R>, void, std::vector<R>> emit(Args... args) {
    if constexpr (std::is_void_v<R>) {
      // Call all callbacks, ignore results
      for (auto it = callbacks_.begin(); it != callbacks_.end();) {
        auto current = it++;
        std::invoke(*current, args...);
      }
    } else {
      // Collect all results into a vector<R>
      std::vector<R> results;
      for (auto it = callbacks_.begin(); it != callbacks_.end();) {
        auto current = it++;
        results.push_back(std::invoke(*current, args...));
      }
      return results;
    }
  }

 private:
  std::list<Callback> callbacks_;
};

}  // namespace AelkeyUtil

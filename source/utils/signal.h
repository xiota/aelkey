#pragma once

#include <functional>
#include <list>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

namespace AelkeyUtil {

template <typename Signature>
class Signal;

template <typename R, typename... Args>
class Signal<R(Args...)> {
 private:
  struct SignalData {
    std::list<std::function<R(Args...)>> callbacks;
  };

  std::shared_ptr<SignalData> data_ = std::make_shared<SignalData>();

 public:
  using Callback = std::function<R(Args...)>;
  using CallbackIt = typename std::list<Callback>::iterator;

  class Connection {
   public:
    Connection() = default;

    Connection(std::shared_ptr<SignalData> data, CallbackIt it)
        : data_(std::move(data)), it_(it), active_(true) {}

    Connection(const Connection &) = delete;
    Connection &operator=(const Connection &) = delete;

    Connection(Connection &&other) noexcept
        : data_(std::move(other.data_)), it_(other.it_), active_(other.active_) {
      other.active_ = false;
    }

    Connection &operator=(Connection &&other) noexcept {
      if (this != &other) {
        disconnect();
        data_ = std::move(other.data_);
        it_ = other.it_;
        active_ = other.active_;
        other.active_ = false;
      }
      return *this;
    }

    ~Connection() {
      disconnect();
    }

    void disconnect() {
      if (active_ && data_) {
        data_->callbacks.erase(it_);
        active_ = false;
        data_.reset();
      }
    }

    bool connected() const noexcept {
      return active_;
    }

   private:
    friend class Signal;
    std::shared_ptr<SignalData> data_;
    CallbackIt it_{};
    bool active_ = false;
  };

  Signal() = default;
  Signal(const Signal &) = delete;
  Signal &operator=(const Signal &) = delete;
  Signal(Signal &&) = default;
  Signal &operator=(Signal &&) = default;

  Connection subscribe(Callback cb) {
    auto it = data_->callbacks.insert(data_->callbacks.end(), std::move(cb));
    return Connection(data_, it);
  }

  std::conditional_t<std::is_void_v<R>, void, std::vector<R>> emit(Args... args) {
    if constexpr (std::is_void_v<R>) {
      for (auto it = data_->callbacks.begin(); it != data_->callbacks.end();) {
        auto current = it++;
        std::invoke(*current, args...);
      }
    } else {
      std::vector<R> results;
      for (auto it = data_->callbacks.begin(); it != data_->callbacks.end();) {
        auto current = it++;
        results.push_back(std::invoke(*current, args...));
      }
      return results;
    }
  }
};

}  // namespace AelkeyUtil

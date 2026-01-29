#pragma once

template <typename T>
class Singleton {
 public:
  static T &instance() {
    static T inst;

    if (inst.auto_init_) {
      inst.lazy_init();
    }

    return inst;
  }

  bool lazy_init() {
    if (!initialized_) {
      initialized_ = static_cast<T *>(this)->on_init();
    }
    return initialized_;
  }

 protected:
  Singleton() = default;
  virtual ~Singleton() = default;

  Singleton(const Singleton &) = delete;
  Singleton &operator=(const Singleton &) = delete;
  Singleton(Singleton &&) = delete;
  Singleton &operator=(Singleton &&) = delete;

 protected:
  virtual bool on_init() {
    return true;
  }

  bool auto_init_ = false;

 private:
  bool initialized_ = false;
};

#ifndef BLOX7_WINDOW_WINDOW_FULLSCREEN_STATE_SYNC_H_
#define BLOX7_WINDOW_WINDOW_FULLSCREEN_STATE_SYNC_H_

#include <condition_variable>
#include <cstddef>
#include <mutex>

namespace blox7 {
namespace window {

using FullscreenStateSyncCallback = bool (*)(void* context, bool fullscreen);

class WindowFullscreenStateSync final {
 public:
  bool Register(FullscreenStateSyncCallback callback, void* context) {
    if (callback == nullptr || context == nullptr) {
      return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (callback_ != nullptr || clearing_) {
      return false;
    }
    callback_ = callback;
    context_ = context;
    return true;
  }

  bool Notify(bool fullscreen) {
    FullscreenStateSyncCallback callback = nullptr;
    void* context = nullptr;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (callback_ == nullptr) {
        return true;
      }
      callback = callback_;
      context = context_;
      ++in_flight_;
    }
    const bool success = callback(context, fullscreen);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      --in_flight_;
    }
    condition_.notify_all();
    return success;
  }

  void Clear() {
    std::unique_lock<std::mutex> lock(mutex_);
    condition_.wait(lock, [this] { return !clearing_; });
    clearing_ = true;
    callback_ = nullptr;
    context_ = nullptr;
    condition_.wait(lock, [this] { return in_flight_ == 0; });
    clearing_ = false;
    lock.unlock();
    condition_.notify_all();
  }

 private:
  std::mutex mutex_;
  std::condition_variable condition_;
  FullscreenStateSyncCallback callback_ = nullptr;
  void* context_ = nullptr;
  std::size_t in_flight_ = 0;
  bool clearing_ = false;
};

}  // namespace window
}  // namespace blox7

#endif  // BLOX7_WINDOW_WINDOW_FULLSCREEN_STATE_SYNC_H_

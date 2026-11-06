#ifndef BLOX7_WINDOW_WINDOW_FULLSCREEN_REQUEST_GATE_H_
#define BLOX7_WINDOW_WINDOW_FULLSCREEN_REQUEST_GATE_H_

#include <cstdint>
#include <mutex>

namespace blox7 {
namespace window {

inline constexpr std::uint32_t kAndroidWindowFlagFullscreen = 0x00000400U;
inline constexpr std::uint32_t kAndroidWindowFlagForceNotFullscreen =
    0x00000800U;

class WindowFullscreenRequestGate final {
 public:
  bool RequestFromAndroidFlags(int flags, int mask);
  bool Take(bool* fullscreen);
  void Reset();

 private:
  std::mutex mutex_;
  std::uint32_t android_flags_ = 0;
  int pending_state_ = 0;
};

}  // namespace window
}  // namespace blox7

#endif  // BLOX7_WINDOW_WINDOW_FULLSCREEN_REQUEST_GATE_H_

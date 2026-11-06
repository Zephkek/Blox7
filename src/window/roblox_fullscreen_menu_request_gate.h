#ifndef BLOX7_WINDOW_ROBLOX_FULLSCREEN_MENU_REQUEST_GATE_H_
#define BLOX7_WINDOW_ROBLOX_FULLSCREEN_MENU_REQUEST_GATE_H_

#include <cstdint>
#include <mutex>

namespace blox7 {
namespace window {

class RobloxFullscreenMenuRequestGate final {
 public:
  bool RequestFromAndroidLog(const char* tag, const char* message,
                             std::uint64_t monotonic_ns);
  bool Take(std::uint64_t monotonic_ns);
  void Reset();

 private:
  std::mutex mutex_;
  std::uint64_t last_request_ns_ = 0;
  std::uint64_t suppress_until_ns_ = 0;
  bool pending_ = false;
};

}  // namespace window
}  // namespace blox7

#endif  // BLOX7_WINDOW_ROBLOX_FULLSCREEN_MENU_REQUEST_GATE_H_

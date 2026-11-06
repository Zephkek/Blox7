#ifndef BLOX7_RUNTIME_ROBLOX_FULLSCREEN_RUNTIME_BRIDGE_H_
#define BLOX7_RUNTIME_ROBLOX_FULLSCREEN_RUNTIME_BRIDGE_H_

#include <cstdint>

#include "compat/build_profile.h"
#include "blox7/status.h"

namespace blox7 {
namespace runtime {

class RobloxFullscreenRuntimeBridge final {
 public:
  RobloxFullscreenRuntimeBridge() = default;
  ~RobloxFullscreenRuntimeBridge();

  RobloxFullscreenRuntimeBridge(const RobloxFullscreenRuntimeBridge&) =
      delete;
  RobloxFullscreenRuntimeBridge& operator=(
      const RobloxFullscreenRuntimeBridge&) = delete;

  Status Install(const compat::BuildProfile& profile);
  void Shutdown();
  bool installed() const { return installed_; }

 private:
  static void ObserveAndroidLog(int priority, const char* tag,
                                const char* message);
  static bool SynchronizeState(void* context, bool fullscreen);
  bool ApplyState(bool fullscreen);

  std::uintptr_t setter_rva_ = 0;
  std::uintptr_t library_base_ = 0;
  bool setter_validated_ = false;
  bool installed_ = false;
};

}  // namespace runtime
}  // namespace blox7

#endif  // BLOX7_RUNTIME_ROBLOX_FULLSCREEN_RUNTIME_BRIDGE_H_

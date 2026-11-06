#ifndef BLOX7_WINDOW_VIDEO_DRIVER_POLICY_H_
#define BLOX7_WINDOW_VIDEO_DRIVER_POLICY_H_

#include <filesystem>
#include <string_view>
#include <vector>

namespace blox7 {
namespace window {

enum class VideoDriverChoice {
  kSdlDefault,
  kWayland,
  kX11,
  kNvidiaDirectVulkanX11,
};

struct VideoDriverPolicyInput {
  bool has_explicit_sdl_driver = false;
  bool force_wayland = false;
  bool force_x11 = false;
  bool prefer_wayland = true;
  bool has_wayland_session = false;
  bool has_x11_display = false;
  bool uses_direct_vulkan = false;
  bool has_nvidia_kernel_driver = false;
};

VideoDriverChoice ResolveVideoDriverChoice(const VideoDriverPolicyInput& input);

const char* VideoDriverChoiceName(VideoDriverChoice choice);

bool HasNvidiaKernelDriver(
    const std::filesystem::path& proc_version = "/proc/driver/nvidia/version",
    const std::filesystem::path& pci_driver = "/sys/bus/pci/drivers/nvidia");

bool HasAvailableVideoDriverCandidate(
    std::string_view requested,
    const std::vector<std::string_view>& available_drivers);

}  // namespace window
}  // namespace blox7

#endif  // BLOX7_WINDOW_VIDEO_DRIVER_POLICY_H_

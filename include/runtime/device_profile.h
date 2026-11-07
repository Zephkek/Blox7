#ifndef BLOX7_RUNTIME_DEVICE_PROFILE_H_
#define BLOX7_RUNTIME_DEVICE_PROFILE_H_

#include <cstddef>
#include <string>
#include <string_view>

namespace blox7 {
namespace runtime {

enum class DeviceClass {
  kPc,
  kMobile,
  kConsole,
};

struct DeviceProfile {
  std::string name;
  std::string cache_key;
  DeviceClass device_class = DeviceClass::kPc;
  std::string platform_name;
  std::string display_name;
  std::string manufacturer;
  std::string model;
  std::string brand;
  std::string device_code;
  std::string device_sku;
  std::string soc_model;
  std::string roblox_http_user_agent;
  bool touch_enabled = false;
  bool mouse_enabled = true;
  bool keyboard_enabled = true;
  bool pc_hardware = true;
  // the phone has a screen to poke, the pc running it usually doesnt
  bool touch_follows_host = false;
  int display_width = 0;
  int display_height = 0;
  int xdpi = 0;
  int ydpi = 0;
  int width_dp = 0;
  int height_dp = 0;
  unsigned long long total_memory_mb = 0;
  std::string release{};
};

inline constexpr std::string_view kDefaultDeviceProfileName = "mobile-s22-ultra";
inline constexpr std::string_view kRobloxDesktopHttpUserAgent =
    "Roblox/WinInet";
// the real client's ua, built like the apk's cl/i.c()
inline constexpr std::string_view kSamsungS22UltraHttpUserAgent =
    "Mozilla/5.0 (10515MB; 1440x3088; 501x499; 384x823; Samsung SM-S908B; 16) "
    "AppleWebKit/537.36 (KHTML, like Gecko)  ROBLOX Android App {roblox_version} Phone "
    "Hybrid()  GooglePlayStore RobloxApp/{roblox_version} (GlobalDist; GooglePlayStore)";
inline constexpr std::string_view kRobloxConsoleAdmissionUserAgent =
    "Roblox/XboxOne";

const DeviceProfile* FindDeviceProfile(std::string_view name);
bool IsValidDeviceProfileValue(std::string_view value, std::size_t maximum);
std::string BuildCustomDeviceProfileCacheKey(const DeviceProfile& profile);
std::string BuildDeviceHttpUserAgent(const DeviceProfile& profile);
std::string_view DeviceClassName(DeviceClass device_class);

}  // namespace runtime
}  // namespace blox7

#endif  // BLOX7_RUNTIME_DEVICE_PROFILE_H_

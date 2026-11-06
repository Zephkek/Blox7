#include "runtime/device_profile.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace blox7 {
namespace runtime {
namespace {

DeviceProfile MobileProfile(std::string name, std::string display_name,
                            std::string manufacturer, std::string model,
                            std::string brand, std::string device_code,
                            std::string device_sku, std::string soc_model,
                            int display_width, int display_height, int xdpi,
                            int ydpi, int width_dp, int height_dp,
                            unsigned long long total_memory_mb,
                            std::string release) {
  DeviceProfile profile;
  profile.name = std::move(name);
  profile.cache_key = profile.name;
  profile.device_class = DeviceClass::kMobile;
  profile.platform_name = "Android";
  profile.display_name = std::move(display_name);
  profile.manufacturer = std::move(manufacturer);
  profile.model = std::move(model);
  profile.brand = std::move(brand);
  profile.device_code = std::move(device_code);
  profile.device_sku = std::move(device_sku);
  profile.soc_model = std::move(soc_model);
  profile.touch_enabled = true;
  profile.mouse_enabled = true;
  profile.keyboard_enabled = true;
  profile.pc_hardware = false;
  profile.touch_follows_host = true;
  profile.display_width = display_width;
  profile.display_height = display_height;
  profile.xdpi = xdpi;
  profile.ydpi = ydpi;
  profile.width_dp = width_dp;
  profile.height_dp = height_dp;
  profile.total_memory_mb = total_memory_mb;
  profile.release = std::move(release);
  return profile;
}

std::vector<DeviceProfile> MakeProfiles() {
  std::vector<DeviceProfile> profiles;
  profiles.push_back({
      "pc-windows-11",
      "pc-windows-11",
      DeviceClass::kPc,
      "Windows",
      "Windows 11 PC",
      "Microsoft",
      "Windows 11 PC",
      "Microsoft",
      "windows11",
      "windows-11-x86_64",
      "x86_64",
      std::string(kRobloxDesktopHttpUserAgent),
      false,
      true,
      true,
      true,
  });
  DeviceProfile s22_ultra = MobileProfile(
      "mobile-s22-ultra", "Samsung Galaxy S22 Ultra", "samsung", "SM-S908B",
      "samsung", "b0s", "b0sxeea", "s5e9925", 1440, 3088, 501, 499, 384, 823,
      10515, "16");
  s22_ultra.roblox_http_user_agent = std::string(kSamsungS22UltraHttpUserAgent);
  profiles.push_back(std::move(s22_ultra));
  profiles.push_back(MobileProfile("mobile-s22", "Samsung Galaxy S22", "samsung",
                                   "SM-S901B", "samsung", "r0s", "r0sxeea",
                                   "s5e9925", 1080, 2340, 425, 425, 384, 832,
                                   8000, "16"));
  profiles.push_back(MobileProfile(
      "mobile-s23-ultra", "Samsung Galaxy S23 Ultra", "samsung", "SM-S918B",
      "samsung", "dm3q", "dm3qxxx", "SM8550-AC", 1440, 3088, 500, 500, 384,
      823, 12000, "16"));
  profiles.push_back(MobileProfile(
      "mobile-s24-ultra", "Samsung Galaxy S24 Ultra", "samsung", "SM-S928B",
      "samsung", "e3q", "e3qxxx", "SM8650-AC", 1440, 3120, 505, 505, 384, 832,
      12000, "16"));
  profiles.push_back(MobileProfile("mobile-pixel-7", "Google Pixel 7", "Google",
                                   "Pixel 7", "google", "panther", "panther",
                                   "Google Tensor G2", 1080, 2400, 420, 420,
                                   412, 915, 8000, "16"));
  profiles.push_back(MobileProfile("mobile-pixel-8", "Google Pixel 8", "Google",
                                   "Pixel 8", "google", "shiba", "shiba",
                                   "Google Tensor G3", 1080, 2400, 420, 420,
                                   412, 915, 8000, "16"));
  profiles.push_back(MobileProfile(
      "mobile-oneplus-11", "OnePlus 11", "OnePlus", "PHB110", "OnePlus",
      "salami", "salami", "SM8550", 1440, 3216, 525, 525, 412, 920, 16000,
      "16"));
  profiles.push_back(MobileProfile("mobile-xiaomi-13", "Xiaomi 13", "Xiaomi",
                                   "2211133G", "Xiaomi", "fuxi", "fuxi",
                                   "SM8550", 1080, 2400, 440, 440, 393, 873,
                                   12000, "16"));
  profiles.push_back(MobileProfile(
      "mobile-a54", "Samsung Galaxy A54 5G", "samsung", "SM-A546B", "samsung",
      "a54x", "a54xeea", "s5e8835", 1080, 2340, 405, 405, 384, 832, 8000,
      "16"));
  profiles.push_back({
      "console-ps5",
      "console-ps5",
      DeviceClass::kConsole,
      "PlayStation",
      "PlayStation 5",
      "Sony Interactive Entertainment",
      "PlayStation 5",
      "PlayStation",
      "prospero",
      "ps5",
      "AMD custom Zen 2 / RDNA 2",
      std::string(kRobloxConsoleAdmissionUserAgent),
      false,
      true,
      true,
      false,
  });
  return profiles;
}

const std::vector<DeviceProfile> kProfiles = MakeProfiles();

std::string_view CanonicalName(std::string_view name) {
  if (name == "pc") {
    return "pc-windows-11";
  }
  if (name == "mobile") {
    return "mobile-pixel-8";
  }
  if (name == "phone" || name == "s22" || name == "samsung" ||
      name == "samsung-s22-ultra") {
    return "mobile-s22-ultra";
  }
  if (name == "s23" || name == "samsung-s23-ultra") {
    return "mobile-s23-ultra";
  }
  if (name == "s24" || name == "samsung-s24-ultra") {
    return "mobile-s24-ultra";
  }
  if (name == "pixel" || name == "pixel-8") {
    return "mobile-pixel-8";
  }
  if (name == "pixel-7") {
    return "mobile-pixel-7";
  }
  if (name == "oneplus" || name == "oneplus-11") {
    return "mobile-oneplus-11";
  }
  if (name == "xiaomi" || name == "xiaomi-13") {
    return "mobile-xiaomi-13";
  }
  if (name == "a54" || name == "samsung-a54") {
    return "mobile-a54";
  }
  if (name == "console" || name == "console-xbox-series-x") {
    return "console-ps5";
  }
  return name;
}

}  // namespace

std::string BuildDeviceHttpUserAgent(const DeviceProfile& profile) {
  std::string model = profile.model.rfind(profile.manufacturer, 0) == 0
                          ? profile.model
                          : profile.manufacturer + " " + profile.model;
  if (!model.empty() && model[0] >= 'a' && model[0] <= 'z') {
    model[0] = static_cast<char>(model[0] - 'a' + 'A');
  }
  char block[512];
  std::snprintf(block, sizeof block, "(%lluMB; %dx%d; %dx%d; %dx%d; %s; %s)",
                profile.total_memory_mb, profile.display_width,
                profile.display_height, profile.xdpi, profile.ydpi,
                profile.width_dp, profile.height_dp, model.c_str(),
                profile.release.c_str());
  return "Mozilla/5.0 " + std::string(block) +
         " AppleWebKit/537.36 (KHTML, like Gecko)  ROBLOX Android App "
         "{roblox_version} Phone Hybrid()  GooglePlayStore "
         "RobloxApp/{roblox_version} (GlobalDist; GooglePlayStore)";
}

const DeviceProfile* FindDeviceProfile(std::string_view name) {
  const std::string_view canonical = CanonicalName(name);
  for (const DeviceProfile& profile : kProfiles) {
    if (profile.name == canonical) {
      return &profile;
    }
  }
  return nullptr;
}

bool IsValidDeviceProfileValue(std::string_view value, std::size_t maximum) {
  if (value.empty() || value.size() > maximum) {
    return false;
  }
  for (const unsigned char character : value) {
    if (character < 0x20U || character == 0x7fU) {
      return false;
    }
  }
  return true;
}

std::string BuildCustomDeviceProfileCacheKey(const DeviceProfile& profile) {
  std::uint64_t hash = UINT64_C(14695981039346656037);
  const auto consume = [&hash](std::string_view value) {
    for (const unsigned char character : value) {
      hash ^= character;
      hash *= UINT64_C(1099511628211);
    }
    hash ^= UINT64_C(0xff);
    hash *= UINT64_C(1099511628211);
  };
  consume(profile.name);
  consume(profile.platform_name);
  consume(profile.display_name);
  consume(profile.manufacturer);
  consume(profile.model);
  consume(profile.brand);
  consume(profile.device_code);
  consume(profile.device_sku);
  consume(profile.soc_model);

  constexpr char kHex[] = "0123456789abcdef";
  std::array<char, 16> encoded{};
  for (std::size_t index = 0; index < encoded.size(); ++index) {
    const std::size_t shift = (encoded.size() - index - 1) * 4;
    encoded[index] = kHex[(hash >> shift) & 0xfU];
  }
  return profile.name + "-custom-" +
         std::string(encoded.data(), encoded.size());
}

std::string_view DeviceClassName(DeviceClass device_class) {
  switch (device_class) {
    case DeviceClass::kPc:
      return "pc";
    case DeviceClass::kMobile:
      return "mobile";
    case DeviceClass::kConsole:
      return "console";
  }
  return "unknown";
}

}  // namespace runtime
}  // namespace blox7

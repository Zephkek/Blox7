#include "runtime/graphics_launch_policy.h"

#include <cstdlib>
#include <string>

namespace blox7 {
namespace runtime {
namespace {

constexpr char kVulkanClientSettingsOverrides[] =
    R"({"FStringGraphicsVulkanShaderMTDenyPattern":"4318:.*"})";

bool SetValue(const char* name, const std::string& value, std::string* error) {
  if (setenv(name, value.c_str(), 1) == 0) {
    return true;
  }
  if (error != nullptr) {
    *error = std::string("cannot publish resolved graphics setting: ") + name;
  }
  return false;
}

bool SetDefault(const char* name, const std::string& value,
                std::string* error) {
  const char* current = std::getenv(name);
  if (current != nullptr && current[0] != '\0') {
    return true;
  }
  return SetValue(name, value, error);
}

}  // namespace

bool ApplyGraphicsLaunchPolicy(const RuntimeConfig& config,
                               std::string* error) {
  if (!RuntimeConfig::IsValidPresentMode(config.present_mode())) {
    if (error != nullptr) *error = "cannot apply an invalid present mode";
    return false;
  }
  if (config.graphics_backend() == GraphicsBackend::kUnknown) {
    if (error != nullptr) {
      *error = "cannot apply an unknown graphics backend";
    }
    return false;
  }

  const bool direct_vulkan =
      config.graphics_backend() == GraphicsBackend::kVulkan;
  if (!SetValue("BLOX7_GRAPHICS_BACKEND",
                config.graphics_backend_name(), error) ||
      !SetValue("BLOX7_PRELOAD_VULKAN_SHIM",
                direct_vulkan ? "1" : "0", error) ||
      !SetDefault("BLOX7_REQUIRE_REAL_GRAPHICS", "1", error)) {
    return false;
  }

  if (direct_vulkan &&
      (!SetDefault("BLOX7_CLIENT_SETTINGS_OVERRIDES_JSON",
                   kVulkanClientSettingsOverrides, error) ||
       !SetDefault("VK_LOADER_DRIVERS_DISABLE", "lvp_icd:dzn_icd:virtio_icd",
                   error))) {
    return false;
  }
  return true;
}

}  // namespace runtime
}  // namespace blox7

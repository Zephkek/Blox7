#ifndef BLOX7_RUNTIME_GRAPHICS_LAUNCH_POLICY_H_
#define BLOX7_RUNTIME_GRAPHICS_LAUNCH_POLICY_H_

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "runtime/runtime_config.h"

namespace blox7 {
namespace runtime {

std::string SelectVulkanIcdManifest(
    const std::vector<std::filesystem::path>& directories,
    std::string_view vendor);

bool ApplyGraphicsLaunchPolicy(const RuntimeConfig& config,
                               std::string* error = nullptr);

}  // namespace runtime
}  // namespace blox7

#endif  // BLOX7_RUNTIME_GRAPHICS_LAUNCH_POLICY_H_

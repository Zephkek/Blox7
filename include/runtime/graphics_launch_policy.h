#ifndef BLOX7_RUNTIME_GRAPHICS_LAUNCH_POLICY_H_
#define BLOX7_RUNTIME_GRAPHICS_LAUNCH_POLICY_H_

#include <string>

#include "runtime/runtime_config.h"

namespace blox7 {
namespace runtime {

bool ApplyGraphicsLaunchPolicy(const RuntimeConfig& config,
                               std::string* error = nullptr);

}  // namespace runtime
}  // namespace blox7

#endif  // BLOX7_RUNTIME_GRAPHICS_LAUNCH_POLICY_H_

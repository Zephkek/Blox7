#ifndef BLOX7_RUNTIME_SUPPORTED_LAUNCH_POLICY_H_
#define BLOX7_RUNTIME_SUPPORTED_LAUNCH_POLICY_H_

#include <string>

namespace blox7 {
namespace runtime {

bool ApplySupportedLaunchPolicy(bool interactive, std::string* error = nullptr);

}  // namespace runtime
}  // namespace blox7

#endif  // BLOX7_RUNTIME_SUPPORTED_LAUNCH_POLICY_H_

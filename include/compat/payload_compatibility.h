#ifndef BLOX7_COMPAT_PAYLOAD_COMPATIBILITY_H_
#define BLOX7_COMPAT_PAYLOAD_COMPATIBILITY_H_

#include <string>

#include "compat/build_profile.h"

namespace blox7 {
namespace compat {

struct PayloadCompatibilityResult {
  BuildProfile profile;
  std::string build_id;
  std::string error;

  explicit operator bool() const noexcept { return error.empty(); }
};

PayloadCompatibilityResult CheckPayloadCompatibility(
    const std::string& library_path, const std::string& manifest_path,
    bool allow_unverified_build);

}  // namespace compat
}  // namespace blox7

#endif  // BLOX7_COMPAT_PAYLOAD_COMPATIBILITY_H_

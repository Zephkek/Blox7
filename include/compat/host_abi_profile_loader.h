#ifndef BLOX7_COMPAT_HOST_ABI_PROFILE_LOADER_H_
#define BLOX7_COMPAT_HOST_ABI_PROFILE_LOADER_H_

#include <string>

#include "compat/host_abi_profile.h"

namespace blox7::compat {

struct FileSha256Result {
  std::string sha256;
  std::string error;

  explicit operator bool() const noexcept { return error.empty(); }
};

FileSha256Result ComputeFileSha256(const std::string& path);

struct ExternalHostAbiProfileRequest {
  std::string profile_file;
  std::string payload_path;
  std::string compatibility_manifest_path;
  std::string expected_build_id;
  std::string approval_receipt_path;
  bool candidate_canary = false;
  bool candidate_process_authorization = false;
  bool explicit_unverified_authorization = false;
};

struct ExternalHostAbiProfileResult {
  const HostAbiProfile* profile = nullptr;
  std::string payload_sha256;
  std::string profile_sha256;
  std::string error;

  explicit operator bool() const noexcept { return error.empty(); }
};

ExternalHostAbiProfileResult LoadExternalHostAbiProfile(
    const ExternalHostAbiProfileRequest& request);

ExternalHostAbiProfileResult InitializeHostAbiProfileFromEnvironment(
    const std::string& payload_path,
    const std::string& compatibility_manifest_path,
    const std::string& expected_build_id,
    bool explicit_unverified_authorization);

const HostAbiProfile* FindLoadedExternalHostAbiProfile(
    std::string_view build_id) noexcept;

void SetHostAbiProfileHashingFaultForTesting(bool enabled) noexcept;

}  // namespace blox7::compat

#endif  // BLOX7_COMPAT_HOST_ABI_PROFILE_LOADER_H_

#ifndef BLOX7_UPDATE_HOST_ABI_DERIVER_H_
#define BLOX7_UPDATE_HOST_ABI_DERIVER_H_

#include <filesystem>
#include <string>
#include <vector>

namespace blox7::update {

struct HostAbiDerivationOptions {
  std::filesystem::path reference_library;
  std::filesystem::path reference_profile;
  std::vector<std::filesystem::path> reference_compatibility_manifests;
  std::filesystem::path candidate_payload_directory;
  std::filesystem::path output_directory;
};

struct HostAbiDerivationResult {
  std::filesystem::path profile;
  std::filesystem::path compatibility_manifest;
  std::string error;

  explicit operator bool() const {
    return error.empty() && !profile.empty() && !compatibility_manifest.empty();
  }
};

struct HostAbiSidecarIdentity {
  std::string elf_build_id;
  std::string payload_id;
  std::string error;

  explicit operator bool() const {
    return error.empty() && !elf_build_id.empty() && !payload_id.empty();
  }
};

HostAbiSidecarIdentity ReadHostAbiSidecarIdentity(
    const std::filesystem::path& sidecar);

HostAbiDerivationResult DeriveHostAbiProfile(
    const HostAbiDerivationOptions& options);

}  // namespace blox7::update

#endif  // BLOX7_UPDATE_HOST_ABI_DERIVER_H_

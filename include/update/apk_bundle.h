#ifndef BLOX7_UPDATE_APK_BUNDLE_H_
#define BLOX7_UPDATE_APK_BUNDLE_H_

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "update/compatibility_catalog.h"

namespace blox7::update {

struct PreparedPayload {
  std::filesystem::path directory;
  std::string payload_id;
  std::string version_name;
  std::uint64_t version_code = 0;
  std::string build_id;
  std::string error;

  explicit operator bool() const { return error.empty(); }
};

struct ExpectedPayloadIdentity {
  std::string version_name;
  std::uint64_t version_code = 0;
  std::optional<std::string> exact_build_id;
};

PreparedPayload PreparePayloadFromArchives(
    const std::vector<std::filesystem::path>& archives,
    const ExpectedPayloadIdentity& expected,
    const std::filesystem::path& signing_trust_manifest,
    const std::filesystem::path& workspace, std::string_view source);

ExpectedPayloadIdentity ExactPayloadIdentity(
    const SupportedPayloadProfile& profile);

}  // namespace blox7::update

#endif  // BLOX7_UPDATE_APK_BUNDLE_H_

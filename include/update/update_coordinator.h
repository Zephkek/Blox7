#ifndef BLOX7_UPDATE_UPDATE_COORDINATOR_H_
#define BLOX7_UPDATE_UPDATE_COORDINATOR_H_

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "update/apk_bundle.h"
#include "update/apk_provider.h"
#include "update/compatibility_catalog.h"
#include "update/payload_store.h"
#include "update/readiness_canary.h"
#include "update/update_config.h"

namespace blox7::update {

struct UpdatePaths {
  std::filesystem::path config_file;
  std::filesystem::path data_root;
  std::filesystem::path cache_root;
  std::filesystem::path state_root;
  std::filesystem::path compatibility_manifest;
  std::filesystem::path signing_trust_manifest;
  std::filesystem::path download_sources_manifest;
  std::filesystem::path host_abi_reference_profile;
  std::filesystem::path runtime_binary;
  std::filesystem::path runtime_launcher;
};

struct UpdateRequest {
  bool startup_preflight = false;
  bool check_latest = true;
  bool run_canary = true;
  bool download_only = false;
  bool force_run_latest = false;
  CanaryGraphicsBackend canary_graphics_backend =
      CanaryGraphicsBackend::kDirectVulkan;
  int progress_fd = -1;
};

struct UpdateResult {
  bool changed = false;
  bool stale = false;
  std::string payload_id;
  std::string message;
  std::vector<std::string> warnings;
  std::string error;

  explicit operator bool() const { return error.empty(); }
};

struct UpdateCandidatePlan {
  ExpectedPayloadIdentity identity;
  bool exact_supported = false;
  bool reuse_installed = false;
  std::string origin;
};

std::optional<SupportedPayloadProfile> ResolveReferenceProfile(
    const std::filesystem::path& location,
    const std::vector<SupportedPayloadProfile>& profiles,
    std::filesystem::path* sidecar);

std::vector<UpdateCandidatePlan> PlanUpdateCandidates(
    const std::vector<SupportedPayloadProfile>& profiles,
    const SupportedPayloadProfile& preferred, const ProviderVersion* latest,
    const PayloadStoreResult& current, const PayloadStoreResult& installed);

UpdateResult RunUpdate(const UpdatePaths& paths, const UpdateRequest& request,
                       const ApkProvider* source = nullptr);

ProviderChain ConfiguredProviderChain(const UpdateConfig& config);

}  // namespace blox7::update

#endif  // BLOX7_UPDATE_UPDATE_COORDINATOR_H_

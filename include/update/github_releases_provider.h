#ifndef BLOX7_UPDATE_GITHUB_RELEASES_PROVIDER_H_
#define BLOX7_UPDATE_GITHUB_RELEASES_PROVIDER_H_

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "update/apk_provider.h"

namespace blox7::update {

class GitHubReleasesProvider final : public ApkProvider {
 public:
  explicit GitHubReleasesProvider(std::string repository)
      : repository_(std::move(repository)) {}

  std::string_view name() const override { return "github-releases"; }

  ProviderVersion CheckLatest() const override;

  ProviderDownloadResult DownloadExact(
      const ProviderVersion& version,
      const std::filesystem::path& output_directory,
      int progress_fd = -1) const override;

 private:
  std::string repository_;
};

struct GitHubReleaseAsset {
  std::string name;
  std::string url;
  std::uint64_t size = 0;
};

struct GitHubRelease {
  std::string version_name;
  std::uint64_t version_code = 0;
  GitHubReleaseAsset base;
  GitHubReleaseAsset split;
};

std::vector<GitHubRelease> ParseGitHubReleases(std::string_view json,
                                               std::string_view abi,
                                               std::string* error);

bool IsGitHubRepositoryName(std::string_view repository);

}  // namespace blox7::update

#endif  // BLOX7_UPDATE_GITHUB_RELEASES_PROVIDER_H_

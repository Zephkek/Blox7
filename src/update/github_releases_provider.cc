#include "update/github_releases_provider.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "compat/guest_abi.h"
#include "update/http_download.h"

namespace blox7::update {
namespace {

constexpr std::size_t kMaximumListingBytes = 4U * 1024U * 1024U;
constexpr std::size_t kMaximumArchiveBytes = 1024ULL * 1024ULL * 1024ULL;
const std::vector<std::string> kApiHosts = {"api.github.com"};
// release downloads hop from github.com to the asset hosts
const std::vector<std::string> kDownloadHosts = {"github.com",
                                                 "githubusercontent.com"};

bool VersionName(std::string_view value) {
  return !value.empty() && value.size() <= 128 &&
         std::all_of(value.begin(), value.end(), [](unsigned char c) {
           return std::isalnum(c) || c == '.' || c == '+' || c == '_' ||
                  c == '-';
         });
}

bool AssetName(std::string_view value) {
  return value.size() > 4 && value.size() <= 200 &&
         value.substr(value.size() - 4) == ".apk" &&
         std::all_of(value.begin(), value.end(), [](unsigned char c) {
           return std::isalnum(c) || c == '.' || c == '_' || c == '-';
         });
}

std::string StringField(const nlohmann::json& object, const char* key) {
  const auto found = object.find(key);
  return found != object.end() && found->is_string() ? found->get<std::string>()
                                                     : std::string();
}

std::uint64_t NumberField(const nlohmann::json& object, const char* key) {
  const auto found = object.find(key);
  return found != object.end() && found->is_number_unsigned()
             ? found->get<std::uint64_t>()
             : 0;
}

std::uint64_t CodeAfter(std::string_view name, std::string_view prefix) {
  if (name.size() <= prefix.size() + 4 || name.substr(0, prefix.size()) != prefix)
    return 0;
  const std::string_view digits =
      name.substr(prefix.size(), name.size() - prefix.size() - 4);
  std::uint64_t code = 0;
  const auto parsed =
      std::from_chars(digits.data(), digits.data() + digits.size(), code);
  return parsed.ec == std::errc() && parsed.ptr == digits.data() + digits.size()
             ? code
             : 0;
}

std::vector<GitHubRelease> FetchReleases(const std::string& repository,
                                         std::string* error) {
  if (!IsGitHubRepositoryName(repository)) {
    *error = "updates.github_repository is not owner/name: " + repository;
    return {};
  }
  HttpTransferRequest request;
  request.url =
      "https://api.github.com/repos/" + repository + "/releases?per_page=20";
  request.headers = {"Accept: application/vnd.github+json",
                     "X-GitHub-Api-Version: 2022-11-28"};
  request.allowed_hosts = kApiHosts;
  request.maximum_bytes = kMaximumListingBytes;
  request.connect_timeout_ms = 5000;
  request.transfer_timeout_ms = 20000;
  request.maximum_attempts = 2;
  const HttpBytesResult listing = DownloadBytes(request);
  if (!listing) {
    *error = "GitHub releases of " + repository + ": " + listing.error;
    return {};
  }
  return ParseGitHubReleases(listing.bytes, compat::kGuestAbi, error);
}

bool ZipMagic(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  char magic[4] = {};
  input.read(magic, sizeof(magic));
  return input.gcount() == 4 && magic[0] == 'P' && magic[1] == 'K' &&
         magic[2] == 3 && magic[3] == 4;
}

bool HasWholeAsset(const std::filesystem::path& path,
                   const GitHubReleaseAsset& asset) {
  std::error_code error;
  const auto size = std::filesystem::file_size(path, error);
  return !error && asset.size != 0 && size == asset.size && ZipMagic(path);
}

}  // namespace

bool IsGitHubRepositoryName(std::string_view repository) {
  const std::size_t slash = repository.find('/');
  if (slash == std::string_view::npos || slash == 0 || slash > 39 ||
      repository.find('/', slash + 1) != std::string_view::npos ||
      repository.size() - slash - 1 == 0 || repository.size() - slash - 1 > 100)
    return false;
  const std::string_view owner = repository.substr(0, slash);
  const std::string_view name = repository.substr(slash + 1);
  return std::all_of(owner.begin(), owner.end(),
                     [](unsigned char c) { return std::isalnum(c) || c == '-'; }) &&
         std::all_of(name.begin(), name.end(), [](unsigned char c) {
           return std::isalnum(c) || c == '.' || c == '_' || c == '-';
         }) &&
         name != "." && name != "..";
}

std::vector<GitHubRelease> ParseGitHubReleases(std::string_view json,
                                               std::string_view abi,
                                               std::string* error) {
  std::vector<GitHubRelease> releases;
  const nlohmann::json root = nlohmann::json::parse(json, nullptr, false);
  if (root.is_discarded() || !root.is_array()) {
    *error = "GitHub releases listing is not a JSON array";
    return releases;
  }
  std::string abi_name(abi);
  std::replace(abi_name.begin(), abi_name.end(), '-', '_');
  const std::string base_prefix = "roblox-base-vc";
  const std::string split_prefix = "roblox-" + abi_name + "-vc";
  for (const nlohmann::json& release : root) {
    if (!release.is_object()) continue;
    const auto draft = release.find("draft");
    if (draft != release.end() && draft->is_boolean() && draft->get<bool>()) continue;
    std::string version = StringField(release, "tag_name");
    if (!version.empty() && (version.front() == 'v' || version.front() == 'V'))
      version.erase(0, 1);
    const auto assets = release.find("assets");
    if (!VersionName(version) || assets == release.end() || !assets->is_array())
      continue;
    std::map<std::uint64_t, GitHubReleaseAsset> bases, splits;
    for (const nlohmann::json& asset : *assets) {
      if (!asset.is_object()) continue;
      GitHubReleaseAsset entry{StringField(asset, "name"),
                               StringField(asset, "browser_download_url"),
                               NumberField(asset, "size")};
      if (!AssetName(entry.name) || !IsTrustedHttpsUrl(entry.url, kDownloadHosts))
        continue;
      if (const std::uint64_t code = CodeAfter(entry.name, base_prefix))
        bases[code] = entry;
      else if (const std::uint64_t code = CodeAfter(entry.name, split_prefix))
        splits[code] = entry;
    }
    for (const auto& [code, base] : bases) {
      const auto split = splits.find(code);
      if (split != splits.end())
        releases.push_back(GitHubRelease{version, code, base, split->second});
    }
  }
  std::sort(releases.begin(), releases.end(),
            [](const GitHubRelease& a, const GitHubRelease& b) {
              return a.version_code > b.version_code;
            });
  if (releases.empty() && error->empty())
    *error = "no GitHub release carries a Roblox base APK with its " +
             std::string(abi) + " split";
  return releases;
}

ProviderVersion GitHubReleasesProvider::CheckLatest() const {
  std::string error;
  const std::vector<GitHubRelease> releases = FetchReleases(repository_, &error);
  if (releases.empty()) return ProviderVersion{{}, 0, error};
  return ProviderVersion{releases.front().version_name,
                         releases.front().version_code, {}};
}

ProviderDownloadResult GitHubReleasesProvider::DownloadExact(
    const ProviderVersion& version,
    const std::filesystem::path& output_directory, int progress_fd) const {
  ProviderDownloadResult result;
  result.source = std::string(name());
  if (!version || !VersionName(version.version_name)) {
    result.error = "invalid GitHub release version";
    return result;
  }
  std::error_code filesystem_error;
  std::filesystem::create_directories(output_directory, filesystem_error);
  if (filesystem_error ||
      std::filesystem::is_symlink(output_directory, filesystem_error)) {
    result.error = "cannot create GitHub download directory";
    return result;
  }
  const std::vector<GitHubRelease> releases =
      FetchReleases(repository_, &result.error);
  const auto release = std::find_if(
      releases.begin(), releases.end(), [&](const GitHubRelease& candidate) {
        return candidate.version_code == version.version_code;
      });
  if (release == releases.end()) {
    if (result.error.empty() || !releases.empty())
      result.error = "GitHub releases of " + repository_ + " do not offer Roblox " +
                     version.version_name + " (" +
                     std::to_string(version.version_code) + ")";
    return result;
  }
  result.error.clear();
  for (const GitHubReleaseAsset* asset : {&release->base, &release->split}) {
    const std::filesystem::path destination = output_directory / asset->name;
    if (HasWholeAsset(destination, *asset)) {
      result.archives.push_back(destination);
      continue;
    }
    std::error_code size_error;
    const auto have = std::filesystem::file_size(destination, size_error);
    if (!size_error && (asset->size == 0 || have > asset->size))
      std::filesystem::remove(destination, filesystem_error);
    HttpTransferRequest request;
    request.url = asset->url;
    request.allowed_hosts = kDownloadHosts;
    request.maximum_bytes = kMaximumArchiveBytes;
    request.transfer_timeout_ms = 0;
    const HttpDownloadResult downloaded =
        DownloadFile(request, destination, progress_fd);
    if (!downloaded || !HasWholeAsset(destination, *asset)) {
      std::filesystem::remove(destination, filesystem_error);
      result.error = downloaded ? asset->name + " from GitHub is incomplete"
                                : downloaded.error;
      result.archives.clear();
      return result;
    }
    result.archives.push_back(destination);
  }
  return result;
}

}  // namespace blox7::update

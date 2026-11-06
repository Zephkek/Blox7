#include "update/local_apk_provider.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "compat/guest_abi.h"
#include "update/android_manifest.h"
#include "update/zip_archive.h"

namespace blox7::update {
namespace {

constexpr std::size_t kMaximumManifestBytes = 16U * 1024U * 1024U;
constexpr std::size_t kMaximumFiles = 64;

struct LocalApk {
  std::filesystem::path path;
  AndroidManifestIdentity identity;
  bool has_guest_library = false;
};

std::string GuestLibEntryName() {
  return std::string(compat::kGuestLibApkEntryPrefix) +
         std::string(compat::kGuestAbi) + "/libroblox.so";
}

// no symlinks, a link could point anywhere
std::vector<LocalApk> ScanDirectory(const std::filesystem::path& directory,
                                    std::string* error) {
  std::vector<LocalApk> apks;
  std::error_code filesystem_error;
  if (!directory.is_absolute() ||
      !std::filesystem::is_directory(directory, filesystem_error)) {
    *error = "APK directory is not an absolute directory: " + directory.string();
    return apks;
  }
  std::filesystem::directory_iterator iterator(
      directory, std::filesystem::directory_options::none, filesystem_error);
  const std::filesystem::directory_iterator end;
  std::size_t files = 0;
  for (; !filesystem_error && iterator != end;
       iterator.increment(filesystem_error)) {
    const std::filesystem::path& path = iterator->path();
    if (path.extension() != ".apk" ||
        iterator->is_symlink(filesystem_error) ||
        !iterator->is_regular_file(filesystem_error)) {
      continue;
    }
    if (++files > kMaximumFiles) {
      *error = "APK directory holds too many APKs: " + directory.string();
      return {};
    }
    const ZipListResult list = ListZipEntries(path);
    if (!list) continue;
    const ZipReadResult manifest =
        ReadZipEntry(path, "AndroidManifest.xml", kMaximumManifestBytes);
    if (!manifest) continue;
    LocalApk apk;
    apk.identity = ParseAndroidManifest(manifest.bytes);
    if (!apk.identity || apk.identity.package_name != "com.roblox.client" ||
        apk.identity.version_code == 0) {
      continue;
    }
    apk.path = path;
    const std::string guest_library = GuestLibEntryName();
    apk.has_guest_library = std::any_of(
        list.entries.begin(), list.entries.end(),
        [&](const ZipEntry& entry) { return entry.name == guest_library; });
    apks.push_back(std::move(apk));
  }
  if (filesystem_error) {
    *error = "cannot read APK directory: " + directory.string();
    return {};
  }
  return apks;
}

bool Complete(const std::vector<LocalApk>& apks, const LocalApk& base) {
  if (!base.identity.split_name.empty() || base.identity.version_name.empty())
    return false;
  return std::any_of(apks.begin(), apks.end(), [&](const LocalApk& apk) {
    return apk.identity.version_code == base.identity.version_code &&
           apk.has_guest_library &&
           (&apk == &base ||
            apk.identity.split_name == compat::kGuestApkSplitName);
  });
}

}  // namespace

ProviderVersion LocalApkProvider::CheckLatest() const {
  ProviderVersion result;
  const std::vector<LocalApk> apks = ScanDirectory(directory_, &result.error);
  if (!result.error.empty()) return result;
  for (const LocalApk& apk : apks) {
    if (apk.identity.version_code > result.version_code &&
        Complete(apks, apk)) {
      result.version_name = apk.identity.version_name;
      result.version_code = apk.identity.version_code;
    }
  }
  if (!result) {
    result.error = "no Roblox base APK with its " +
                   std::string(compat::kGuestAbi) + " library in " +
                   directory_.string();
  }
  return result;
}

ProviderDownloadResult LocalApkProvider::DownloadExact(
    const ProviderVersion& version, const std::filesystem::path&,
    int) const {
  ProviderDownloadResult result;
  result.source = std::string(name());
  const std::vector<LocalApk> apks = ScanDirectory(directory_, &result.error);
  if (!result.error.empty()) return result;
  for (const LocalApk& apk : apks) {
    if (apk.identity.version_code == version.version_code)
      result.archives.push_back(apk.path);
  }
  if (result.archives.empty()) {
    result.error = "no APK of Roblox " + version.version_name + " (" +
                   std::to_string(version.version_code) + ") in " +
                   directory_.string();
  }
  return result;
}

}  // namespace blox7::update

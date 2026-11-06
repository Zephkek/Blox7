#ifndef BLOX7_UPDATE_APK_PROVIDER_H_
#define BLOX7_UPDATE_APK_PROVIDER_H_

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace blox7::update {

struct ProviderVersion {
  std::string version_name;
  std::uint64_t version_code = 0;
  std::string error;

  explicit operator bool() const {
    return error.empty() && !version_name.empty() && version_code > 0;
  }
};

struct ProviderDownloadResult {
  std::vector<std::filesystem::path> archives;
  std::string source;
  std::string error;

  explicit operator bool() const { return error.empty() && !archives.empty(); }
};

using ArchiveValidator =
    std::function<std::string(const ProviderDownloadResult&)>;

class ApkProvider {
 public:
  virtual ~ApkProvider() = default;

  virtual std::string_view name() const = 0;

  virtual ProviderVersion CheckLatest() const = 0;
  virtual bool HasVersionDiscovery() const { return true; }

  virtual ProviderDownloadResult DownloadExact(
      const ProviderVersion& version,
      const std::filesystem::path& output_directory, int progress_fd) const = 0;

  virtual ProviderDownloadResult DownloadVerified(
      const ProviderVersion& version,
      const std::filesystem::path& output_directory, int progress_fd,
      const ArchiveValidator& validate) const;
};

class ProviderChain final : public ApkProvider {
 public:
  void Add(std::unique_ptr<ApkProvider> provider);
  bool empty() const { return providers_.empty(); }

  std::string_view name() const override { return "provider-chain"; }
  ProviderVersion CheckLatest() const override;
  ProviderDownloadResult DownloadExact(
      const ProviderVersion& version,
      const std::filesystem::path& output_directory,
      int progress_fd) const override;
  ProviderDownloadResult DownloadVerified(
      const ProviderVersion& version,
      const std::filesystem::path& output_directory, int progress_fd,
      const ArchiveValidator& validate) const override;

 private:
  std::vector<std::unique_ptr<ApkProvider>> providers_;
};

}  // namespace blox7::update

#endif  // BLOX7_UPDATE_APK_PROVIDER_H_

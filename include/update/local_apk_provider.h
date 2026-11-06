#ifndef BLOX7_UPDATE_LOCAL_APK_PROVIDER_H_
#define BLOX7_UPDATE_LOCAL_APK_PROVIDER_H_

#include <filesystem>
#include <string_view>
#include <utility>

#include "update/apk_provider.h"

namespace blox7::update {

class LocalApkProvider final : public ApkProvider {
 public:
  explicit LocalApkProvider(std::filesystem::path directory)
      : directory_(std::move(directory)) {}

  std::string_view name() const override { return "local-apks"; }

  ProviderVersion CheckLatest() const override;

  ProviderDownloadResult DownloadExact(
      const ProviderVersion& version,
      const std::filesystem::path& output_directory,
      int progress_fd = -1) const override;

 private:
  std::filesystem::path directory_;
};

}  // namespace blox7::update

#endif  // BLOX7_UPDATE_LOCAL_APK_PROVIDER_H_

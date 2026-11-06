#ifndef BLOX7_UPDATE_APK_SIGNATURE_H_
#define BLOX7_UPDATE_APK_SIGNATURE_H_

#include <filesystem>
#include <string>
#include <vector>

namespace blox7::update {

struct ApkSignatureResult {
  std::vector<std::string> certificate_sha256;
  std::string error;

  explicit operator bool() const { return error.empty(); }
};

ApkSignatureResult VerifyApkSignature(const std::filesystem::path& apk_path);

}  // namespace blox7::update

#endif  // BLOX7_UPDATE_APK_SIGNATURE_H_

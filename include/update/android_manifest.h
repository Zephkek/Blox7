#ifndef BLOX7_UPDATE_ANDROID_MANIFEST_H_
#define BLOX7_UPDATE_ANDROID_MANIFEST_H_

#include <cstdint>
#include <string>
#include <string_view>

namespace blox7::update {

struct AndroidManifestIdentity {
  std::string package_name;
  std::string version_name;
  std::string split_name;
  std::uint64_t version_code = 0;
  std::string error;

  explicit operator bool() const { return error.empty(); }
};

AndroidManifestIdentity ParseAndroidManifest(std::string_view binary_xml);

}  // namespace blox7::update

#endif  // BLOX7_UPDATE_ANDROID_MANIFEST_H_

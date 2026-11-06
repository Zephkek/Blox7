#ifndef BLOX7_RUNTIME_HELPER_ENVIRONMENT_H_
#define BLOX7_RUNTIME_HELPER_ENVIRONMENT_H_

#include <cstring>
#include <vector>

namespace blox7::runtime {

inline std::vector<char*> BuildHelperEnvironment(char* const* inherited) {
  constexpr char kOverrides[] = "BLOX7_CLIENT_SETTINGS_OVERRIDES_JSON=";
  std::vector<char*> result;
  for (char* const* entry = inherited; entry != nullptr && *entry != nullptr;
       ++entry) {
    if (std::strncmp(*entry, kOverrides, sizeof(kOverrides) - 1) != 0) {
      result.push_back(*entry);
    }
  }
  result.push_back(nullptr);
  return result;
}

}  // namespace blox7::runtime

#endif  // BLOX7_RUNTIME_HELPER_ENVIRONMENT_H_

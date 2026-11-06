#ifndef BLOX7_RUNTIME_FLEASION_H_
#define BLOX7_RUNTIME_FLEASION_H_

#include <filesystem>
#include <string>

#include "runtime/runtime_config.h"
#include "runtime/runtime_paths.h"

namespace blox7::runtime {

struct FleasionPreparation {
  std::filesystem::path certificate;
  std::filesystem::path base_bundle;
  std::filesystem::path bundle;
  std::string error;
  explicit operator bool() const { return error.empty(); }
};

FleasionPreparation PrepareFleasion(const RuntimeConfig& config,
                                   const Environment& environment,
                                   const RuntimePaths& paths);

}  // namespace blox7::runtime

#endif

#ifndef BLOX7_GRAPHICS_BIONIC_EGL_BRIDGE_H_
#define BLOX7_GRAPHICS_BIONIC_EGL_BRIDGE_H_

#include <string>
#include <unordered_map>

namespace blox7::graphics {

using EglExportMap = std::unordered_map<std::string, void *>;

class BionicEglBridge {
public:
  BionicEglBridge() = default;
  BionicEglBridge(const BionicEglBridge &) = delete;
  BionicEglBridge &operator=(const BionicEglBridge &) = delete;

  bool Load();

  bool IsLoaded() const { return handle_ != nullptr; }
  void *handle() const { return handle_; }
  const EglExportMap &exports() const { return exports_; }
  const std::string &library_path() const { return library_path_; }
  const std::string &error() const { return error_; }

private:
  void *handle_ = nullptr;
  EglExportMap exports_;
  std::string library_path_;
  std::string error_;
};

} // namespace blox7::graphics

#endif // BLOX7_GRAPHICS_BIONIC_EGL_BRIDGE_H_

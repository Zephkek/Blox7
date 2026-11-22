#include "libc_shim/libc_shim.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <iostream>
#include <limits.h>
#include <unordered_map>
#include <unistd.h>
#include <utility>
#include <vector>

namespace libc_shim {

namespace {

std::unordered_map<std::string, std::string> g_path_mappings;
std::vector<std::string> g_ca_bundle_aliases;
std::unordered_map<std::string, std::string> g_mod_files;

bool g_installed = false;
std::atomic<GuestAllocator> g_guest_allocator{nullptr};

constexpr char kCaBundleOverride[] = "BLOX7_CA_BUNDLE";
constexpr std::array<const char*, 2> kDefaultCaBundles = {
    "/etc/ssl/cert.pem",
    "/etc/ssl/certs/ca-certificates.crt",
};
constexpr std::array<const char*, 8> kAndroidCaBundlePaths = {
    "/data/user/0/com.roblox.client/files/exe/cacert.pem",
    "/data/data/com.roblox.client/files/exe/cacert.pem",
    "/data/user/0/com.roblox.client/files/exe/ssl/cacert.pem",
    "/data/data/com.roblox.client/files/exe/ssl/cacert.pem",
    "ssl/cacert.pem",
    "content/ssl/cacert.pem",
    "rbx_bin/assets/ssl/cacert.pem",
    "rbx_bin/assets/content/ssl/cacert.pem",
};
constexpr std::array<std::pair<const char*, const char*>, 3> kModFiles = {{
    {"BLOX7_CURSOR_ARROW_FILE", "content/textures/Cursors/KeyboardMouse/ArrowFarCursor.png"},
    {"BLOX7_CURSOR_ARROW_FILE", "content/textures/ArrowCursor.png"},
    {"BLOX7_CURSOR_HAND_FILE", "content/textures/Cursors/KeyboardMouse/ArrowCursor.png"},
}};
constexpr std::array<const char*, 11> kModExtensions = {
    ".png", ".jpg", ".jpeg", ".tga", ".dds", ".ktx", ".ttf", ".otf", ".ogg", ".mp3", ".wav",
};

std::string Lowercase(std::string text) {
  for (char& ch : text) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  return text;
}

bool IsModFile(const std::filesystem::path& path) {
  const std::string extension = Lowercase(path.extension().string());
  return std::find(kModExtensions.begin(), kModExtensions.end(), extension) != kModExtensions.end();
}

bool IsReplaceableFont(const std::filesystem::path& path) {
  const std::string name = Lowercase(path.filename().string());
  const std::string extension = Lowercase(path.extension().string());
  return (extension == ".ttf" || extension == ".otf") && name.rfind("noto", 0) != 0 &&
         name != "robloxemoji.ttf" && name != "twemojimozilla.ttf";
}

bool IsEnabled(const char* name) {
  const char* value = std::getenv(name);
  return value != nullptr && value[0] != '\0' && value[0] != '0';
}

const char* GetEnvNonEmpty(const char* name) {
  const char* value = std::getenv(name);
  return value != nullptr && value[0] != '\0' ? value : nullptr;
}

std::string HomePath(const char* suffix) {
  const char* home = std::getenv("HOME");
  return std::string(home && home[0] != '\0' ? home : "/root") +
         (suffix ? suffix : "");
}

std::string RuntimeRoot() {
  const char* runtime_root = std::getenv("BLOX7_RUNTIME_ROOT");
  if (runtime_root != nullptr && runtime_root[0] != '\0') {
    return runtime_root;
  }
  return "runtime/sober";
}

std::string DefaultDataRoot() {
  if (IsEnabled("BLOX7_USE_REAL_SOBER_PATHS")) {
    return HomePath("/.var/app/org.vinegarhq.Sober/data/sober");
  }
  return RuntimeRoot() + "/data";
}

bool PrefixMatchesPath(const std::string& path, const std::string& prefix) {
  if (path.size() < prefix.size()) {
    return false;
  }
  if (path.compare(0, prefix.size(), prefix) != 0) {
    return false;
  }
  return path.size() == prefix.size() || path[prefix.size()] == '/';
}

bool ValidateHostCaBundle(const char* path, std::string* canonical_path) {
  if (path == nullptr || path[0] != '/' || canonical_path == nullptr) {
    return false;
  }

  char resolved_path[PATH_MAX];
  if (::realpath(path, resolved_path) == nullptr || resolved_path[0] != '/') {
    return false;
  }

  const int fd = ::open(resolved_path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return false;
  }
  struct stat file_status {};
  const bool status_available = ::fstat(fd, &file_status) == 0;
  const int close_result = ::close(fd);
  const bool has_read_permission =
      status_available &&
      (file_status.st_mode & (S_IRUSR | S_IRGRP | S_IROTH)) != 0;
  if (!status_available || close_result != 0 || !S_ISREG(file_status.st_mode) ||
      file_status.st_size <= 0 || !has_read_permission) {
    return false;
  }

  *canonical_path = resolved_path;
  return true;
}

bool EnsureDirectory(const std::string& path) {
  if (path.empty()) {
    return false;
  }

  std::string partial;
  size_t index = 0;
  if (path[0] == '/') {
    partial = "/";
    index = 1;
  }

  while (index <= path.size()) {
    const size_t next = path.find('/', index);
    std::string component =
        path.substr(index, next == std::string::npos ? next : next - index);
    if (!component.empty()) {
      if (partial.size() > 1 && partial.back() != '/') {
        partial.push_back('/');
      }
      partial += component;
      if (::mkdir(partial.c_str(), 0755) != 0 && errno != EEXIST) {
        return false;
      }
    }
    if (next == std::string::npos) {
      break;
    }
    index = next + 1;
  }

  return true;
}

void EnsureDefaultDataLayout(const std::string& data_root) {
  EnsureDirectory(data_root);
  EnsureDirectory(data_root + "/files");
  EnsureDirectory(data_root + "/cache");
  EnsureDirectory(data_root + "/shared_prefs");
  EnsureDirectory(data_root + "/rbx-storage");
  EnsureDirectory(data_root + "/appData");
  EnsureDirectory(data_root + "/appData/LocalStorage");
  EnsureDirectory(data_root + "/appData/rbx-storage");
  EnsureDirectory(data_root + "/files/appData");
  EnsureDirectory(data_root + "/files/appData/LocalStorage");
  EnsureDirectory(data_root + "/files/appData/OTAPatchBackups");
  EnsureDirectory(data_root + "/files/appData/rbx-storage");
  EnsureDirectory(data_root + "/cache/ContentProvider_2");
  EnsureDirectory(data_root + "/cache/rbx-storage");
  EnsureDirectory(data_root + "/cache/sounds");
  EnsureDirectory(data_root + "/sdcard/Android/data/com.roblox.client");
  EnsureDirectory(data_root + "/sdcard/Android/data/com.roblox.client/files");
  EnsureDirectory(data_root + "/sdcard/Android/data/com.roblox.client/cache");
}

}  // namespace

void ConfigureGuestAllocator(GuestAllocator allocator) noexcept {
  g_guest_allocator.store(allocator, std::memory_order_release);
}

GuestAllocator GuestAllocatorForCalls() noexcept {
  return g_guest_allocator.load(std::memory_order_acquire);
}

HostCaBundleResolution ResolveHostCaBundle() {
  const char* override_path = std::getenv(kCaBundleOverride);
  if (override_path != nullptr) {
    HostCaBundleResolution resolution;
    resolution.from_override = true;
    if (!ValidateHostCaBundle(override_path, &resolution.host_path)) {
      resolution.status = HostCaBundleStatus::kInvalidOverride;
      resolution.host_path.clear();
      return resolution;
    }
    resolution.status = HostCaBundleStatus::kReady;
    return resolution;
  }

  for (const char* candidate : kDefaultCaBundles) {
    HostCaBundleResolution resolution;
    if (ValidateHostCaBundle(candidate, &resolution.host_path)) {
      resolution.status = HostCaBundleStatus::kReady;
      return resolution;
    }
  }
  return {};
}

HostCaBundleResolution ConfigureHostCaBundlePathMappings() {
  for (const auto& alias : g_ca_bundle_aliases) g_path_mappings.erase(alias);
  g_ca_bundle_aliases.assign(kAndroidCaBundlePaths.begin(), kAndroidCaBundlePaths.end());
  if (const char* content = GetEnvNonEmpty("BLOX7_ASSET_PATH")) {
    auto root = std::filesystem::path(content).lexically_normal();
    if (root.filename().empty()) root = root.parent_path();
    g_ca_bundle_aliases.push_back((root / "ssl/cacert.pem").string());
    if (root.filename() == "content")
      g_ca_bundle_aliases.push_back((root.parent_path() / "ssl/cacert.pem").string());
  }

  HostCaBundleResolution resolution = ResolveHostCaBundle();
  if (!resolution.ok()) {
    return resolution;
  }
  for (const auto& alias : g_ca_bundle_aliases)
    RegisterPathMapping(alias, resolution.host_path);
  return resolution;
}

void ConfigureModPathMappings() {
  std::string assets;
  if (const char* content = GetEnvNonEmpty("BLOX7_ASSET_PATH")) {
    auto root = std::filesystem::path(content).lexically_normal();
    if (root.filename().empty()) root = root.parent_path();
    if (root.filename() == "content") assets = root.parent_path().string();
  }
  const auto map = [&assets](const std::string& relative, const std::string& file) {
    g_mod_files[relative] = file;
    if (relative.rfind("content/", 0) == 0) RegisterPathMapping(relative, file);
    RegisterPathMapping("rbx_bin/assets/" + relative, file);
    if (!assets.empty()) RegisterPathMapping(assets + "/" + relative, file);
  };
  std::error_code error;
  if (const char* mods = GetEnvNonEmpty("BLOX7_MODS_DIR")) {
    const std::filesystem::path root(mods);
    std::filesystem::recursive_directory_iterator iterator(root, error);
    const std::filesystem::recursive_directory_iterator end;
    while (!error && iterator != end) {
      if (iterator->is_regular_file(error) && IsModFile(iterator->path()))
        map(iterator->path().lexically_relative(root).generic_string(), iterator->path().string());
      iterator.increment(error);
    }
    error.clear();
  }
  const char* font = GetEnvNonEmpty("BLOX7_FONT_FILE");
  if (font != nullptr && !assets.empty() && access(font, R_OK) == 0) {
    for (const std::string folder : {"content/fonts", "fonts"}) {
      std::filesystem::directory_iterator iterator(assets + "/" + folder, error);
      const std::filesystem::directory_iterator end;
      while (!error && iterator != end) {
        if (IsReplaceableFont(iterator->path())) map(folder + "/" + iterator->path().filename().string(), font);
        iterator.increment(error);
      }
      error.clear();
    }
  }
  for (const auto& [variable, relative] : kModFiles) {
    const char* file = GetEnvNonEmpty(variable);
    if (file != nullptr && access(file, R_OK) == 0) map(relative, file);
  }
}

const char* HostCaBundleStatusName(HostCaBundleStatus status) {
  switch (status) {
    case HostCaBundleStatus::kReady:
      return "Ready";
    case HostCaBundleStatus::kInvalidOverride:
      return "InvalidOverride";
    case HostCaBundleStatus::kUnavailable:
      return "Unavailable";
  }
  return "Unknown";
}

void Install() {
  if (g_installed) {
    return;
  }
  g_installed = true;

  const std::string data_root = DefaultDataRoot();

  RegisterPathMapping("/data/user/0/com.roblox.client", data_root);
  RegisterPathMapping("/data/data/com.roblox.client", data_root);
  const HostCaBundleResolution ca_bundle =
      ConfigureHostCaBundlePathMappings();
  if (ca_bundle.ok()) {
    std::cout << "  [libc_shim] Host CA bundle mapped from "
              << (ca_bundle.from_override ? "validated override"
                                          : "system trust store")
              << '\n';
  } else {
    std::cerr << "  [libc_shim] Host CA bundle unavailable: "
              << HostCaBundleStatusName(ca_bundle.status)
              << "; TLS verification remains fail-closed\n";
  }
  const char* cache_root = GetEnvNonEmpty("BLOX7_ANDROID_CACHE_HOST_ROOT");
  if (cache_root != nullptr) {
    RegisterPathMapping("/data/user/0/com.roblox.client/cache", cache_root);
    RegisterPathMapping("/data/data/com.roblox.client/cache", cache_root);
    EnsureDirectory(cache_root);
  }
  const char* shader_cache_path =
      GetEnvNonEmpty("BLOX7_VULKAN_SHADER_CACHE_HOST_PATH");
  if (shader_cache_path != nullptr) {
    RegisterPathMapping(
        "/data/user/0/com.roblox.client/shadercachevk.bin",
        shader_cache_path);
    RegisterPathMapping(
        "/data/data/com.roblox.client/shadercachevk.bin",
        shader_cache_path);
  }
  const char* asset_content_root = GetEnvNonEmpty("BLOX7_ASSET_PATH");
  if (asset_content_root != nullptr) {
    std::string content_root(asset_content_root);
    while (content_root.size() > 1 && content_root.back() == '/') {
      content_root.pop_back();
    }
    const std::string content_suffix = "/content";
    if (content_root.size() > content_suffix.size() &&
        content_root.compare(content_root.size() - content_suffix.size(),
                             content_suffix.size(), content_suffix) == 0) {
      const std::string assets_root = content_root.substr(
          0, content_root.size() - content_suffix.size());
      RegisterPathMapping("content", content_root);
      RegisterPathMapping("rbx_bin/assets/content", content_root);
      RegisterPathMapping("rbx_bin/assets", assets_root);
    }
  }
  ConfigureModPathMappings();
  RegisterPathMapping("/sdcard/Android/data/com.roblox.client",
                      data_root + "/sdcard/Android/data/com.roblox.client");
  RegisterPathMapping("/storage/emulated/0/Android/data/com.roblox.client",
                      data_root + "/sdcard/Android/data/com.roblox.client");
  EnsureDefaultDataLayout(data_root);

  std::cout << "  [libc_shim] Path shims installed ("
            << g_path_mappings.size() << " prefix mappings)\n";
}

std::string TranslatePath(const std::string& android_path) {
  const std::string* best_android_prefix = nullptr;
  const std::string* best_host_prefix = nullptr;
  for (const auto& [android_prefix, host_prefix] : g_path_mappings) {
    if (PrefixMatchesPath(android_path, android_prefix) &&
        (best_android_prefix == nullptr ||
         android_prefix.size() > best_android_prefix->size())) {
      best_android_prefix = &android_prefix;
      best_host_prefix = &host_prefix;
    }
  }
  if (best_android_prefix != nullptr && best_host_prefix != nullptr) {
    return *best_host_prefix +
           android_path.substr(best_android_prefix->size());
  }
  return android_path;
}

void RegisterPathMapping(const std::string& android_prefix,
                         const std::string& host_prefix) {
  g_path_mappings[android_prefix] = host_prefix;
}

}  // namespace libc_shim

namespace {

const char* HostPath(const char* path, std::string* storage) {
  if (path == nullptr) {
    return nullptr;
  }
  *storage = libc_shim::TranslatePath(path);
  return storage->c_str();
}

}  // namespace

extern "C" bool blox7_mod_file(const char* relative, char* file, size_t size) {
  const auto found = libc_shim::g_mod_files.find(relative == nullptr ? "" : relative);
  if (found == libc_shim::g_mod_files.end() || file == nullptr || found->second.size() >= size) return false;
  std::memcpy(file, found->second.c_str(), found->second.size() + 1);
  return true;
}

extern "C" int blox7_open(const char* path, int flags, ...) {
  std::string translated;
  const char* host_path = HostPath(path, &translated);
  int result = -1;
  if ((flags & O_CREAT) != 0) {
    va_list args;
    va_start(args, flags);
    mode_t mode = static_cast<mode_t>(va_arg(args, int));
    va_end(args);
    result = ::open(host_path, flags, mode);
  } else {
    result = ::open(host_path, flags);
  }
  return result;
}

extern "C" int blox7___open_2(const char* path, int flags) {
  std::string translated;
  const char* host_path = HostPath(path, &translated);
  int result = -1;
  if ((flags & O_CREAT) != 0) {
    result = ::open(host_path, flags, 0600);
  } else {
    result = ::open(host_path, flags);
  }
  return result;
}

extern "C" FILE* blox7_fopen(const char* path, const char* mode) {
  std::string translated;
  const char* host_path = HostPath(path, &translated);
  FILE* result = ::fopen(host_path, mode);
  return result;
}

extern "C" int blox7_access(const char* path, int mode) {
  std::string translated;
  const char* host_path = HostPath(path, &translated);
  int result = ::access(host_path, mode);
  return result;
}

extern "C" int blox7_stat(const char* path, struct stat* statbuf) {
  std::string translated;
  const char* host_path = HostPath(path, &translated);
  int result = ::stat(host_path, statbuf);
  return result;
}

extern "C" int blox7_lstat(const char* path, struct stat* statbuf) {
  std::string translated;
  const char* host_path = HostPath(path, &translated);
  int result = ::lstat(host_path, statbuf);
  return result;
}

extern "C" int blox7_statvfs(const char* path, struct statvfs* statbuf) {
  std::string translated;
  return ::statvfs(HostPath(path, &translated), statbuf);
}

extern "C" int blox7_statfs(const char* path, struct statfs* statbuf) {
  std::string translated;
  return ::statfs(HostPath(path, &translated), statbuf);
}

extern "C" int blox7_mkdir(const char* path, mode_t mode) {
  std::string translated;
  return ::mkdir(HostPath(path, &translated), mode);
}

extern "C" DIR* blox7_opendir(const char* path) {
  std::string translated;
  return ::opendir(HostPath(path, &translated));
}

extern "C" int blox7_rename(const char* old_path, const char* new_path) {
  std::string translated_old;
  std::string translated_new;
  return ::rename(HostPath(old_path, &translated_old),
                  HostPath(new_path, &translated_new));
}

extern "C" int blox7_unlink(const char* path) {
  std::string translated;
  return ::unlink(HostPath(path, &translated));
}

extern "C" int blox7_rmdir(const char* path) {
  std::string translated;
  return ::rmdir(HostPath(path, &translated));
}

extern "C" char* blox7_realpath(const char* path, char* resolved_path) {
  std::string translated;
  const char* host_path = HostPath(path, &translated);
  if (resolved_path != nullptr) {
    return ::realpath(host_path, resolved_path);
  }

  char canonical_path[PATH_MAX];
  if (::realpath(host_path, canonical_path) == nullptr) {
    return nullptr;
  }
  const size_t bytes = std::strlen(canonical_path) + 1;
  libc_shim::GuestAllocator allocator = libc_shim::GuestAllocatorForCalls();
  if (allocator == nullptr) {
    return ::strdup(canonical_path);
  }
  auto* result = static_cast<char*>(allocator(bytes));
  if (result == nullptr) {
    errno = ENOMEM;
    return nullptr;
  }
  std::memcpy(result, canonical_path, bytes);
  return result;
}

extern "C" ssize_t blox7_readlink(const char* path, char* buf,
                                      size_t bufsiz) {
  std::string translated;
  return ::readlink(HostPath(path, &translated), buf, bufsiz);
}

extern "C" ssize_t blox7___readlink_chk(const char* path, char* buf,
                                            size_t len, size_t buf_len) {
  if (len > static_cast<size_t>(SSIZE_MAX)) {
    std::fprintf(stderr,
                 "FORTIFY: readlink: size %zu > SSIZE_MAX\n", len);
    std::abort();
  }
  if (len > buf_len) {
    std::fprintf(stderr,
                 "FORTIFY: readlink: prevented %zu-byte write into "
                 "%zu-byte buffer\n",
                 len, buf_len);
    std::abort();
  }
  return blox7_readlink(path, buf, len);
}

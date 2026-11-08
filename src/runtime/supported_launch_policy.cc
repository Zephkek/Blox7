#include "runtime/supported_launch_policy.h"

#include <unistd.h>

#include <array>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#ifndef BLOX7_INSTALL_LIBDIR
#define BLOX7_INSTALL_LIBDIR "lib"
#endif

namespace blox7 {
namespace runtime {
namespace {

bool SetDefault(const char* name, const std::string& value,
                std::string* error) {
  const char* current = std::getenv(name);
  if (current != nullptr && current[0] != '\0') {
    return true;
  }
  if (setenv(name, value.c_str(), 1) == 0) {
    return true;
  }
  if (error != nullptr) {
    *error = std::string("cannot publish supported launch default: ") + name;
  }
  return false;
}

std::filesystem::path CurrentExecutable() {
  std::array<char, 4097> path{};
  const ssize_t length =
      readlink("/proc/self/exe", path.data(), path.size() - 1);
  if (length <= 0 || static_cast<std::size_t>(length) >= path.size()) {
    return {};
  }
  path[static_cast<std::size_t>(length)] = '\0';
  return path.data();
}

bool ApplyInstalledResourceDefaults(std::string* error) {
  const std::filesystem::path executable = CurrentExecutable();
  if (executable.empty()) {
    return true;
  }
  std::filesystem::path runtime_directory = executable.parent_path();
  const std::filesystem::path library_directory = BLOX7_INSTALL_LIBDIR;
  std::filesystem::path prefix = runtime_directory.parent_path();
  if (runtime_directory.filename() == "blox7") {
    for (auto component = library_directory.end();
         component != library_directory.begin();) {
      if (prefix.filename() != *--component) {
        return true;
      }
      prefix = prefix.parent_path();
    }
  } else if (runtime_directory.filename() == "bin") {
    runtime_directory = prefix / library_directory / "blox7";
  } else {
    return true;
  }
#ifdef BLOX7_WIN32
  const std::filesystem::path helper =
      runtime_directory / "blox7_webview_helper.exe";
#else
  const std::filesystem::path helper =
      runtime_directory / "blox7_webview_helper";
#endif
  const std::filesystem::path progress_helper =
      runtime_directory / "blox7_failure_dialog";
  const std::filesystem::path update_helper =
      runtime_directory / "blox7_updater";
  const std::filesystem::path data_directory = prefix / "share/blox7";
  const std::filesystem::path metadata_directory = data_directory / "metadata";
  std::error_code filesystem_error;
  if (!std::filesystem::is_directory(metadata_directory, filesystem_error)) {
    return true;
  }

  const std::vector<std::pair<const char*, std::filesystem::path>> defaults = {
      {"BLOX7_PROJECT_ROOT", data_directory},
      {"BLOX7_COMPATIBILITY_MANIFEST",
       metadata_directory / "roblox_compatibility.json"},
      {"BLOX7_UPDATE_COMPATIBILITY_PATH",
       metadata_directory / "roblox_compatibility.json"},
      {"BLOX7_UPDATE_SIGNING_TRUST_PATH",
       metadata_directory / "roblox_signing_certificates.json"},
      {"BLOX7_UPDATE_DOWNLOAD_SOURCES_PATH",
       metadata_directory / "roblox_download_sources.json"},
      {"BLOX7_BOOTSTRAP_SOURCES_PATH",
       metadata_directory / "roblox_bootstrap_sources.json"},
      {"BLOX7_UPDATE_CANARY_BIN", executable},
      {"BLOX7_UPDATE_HELPER", update_helper},
      {"BLOX7_RUNTIME_LIBRARY_DIR", runtime_directory},
      {"BLOX7_WEBVIEW_HELPER", helper},
      {"BLOX7_UPDATE_PROGRESS_HELPER", progress_helper},
  };
  for (const auto& [name, value] : defaults) {
    if (!SetDefault(name, value.string(), error)) {
      return false;
    }
  }
  return true;
}

}  // namespace

bool ApplySupportedLaunchPolicy(bool interactive, std::string* error) {
  const char* packaged_manifest =
      std::getenv("BLOX7_PACKAGED_COMPATIBILITY_MANIFEST");
  if (packaged_manifest != nullptr && packaged_manifest[0] != '\0' &&
      !SetDefault("BLOX7_COMPATIBILITY_MANIFEST", packaged_manifest, error)) {
    return false;
  }
  if (!ApplyInstalledResourceDefaults(error)) {
    return false;
  }
  if (!interactive) {
    return true;
  }

  const std::vector<std::pair<const char*, std::string>> defaults = {
      {"BLOX7_ALLOW_NO_COOKIE_LUA_APP", "1"},
      {"BLOX7_VALIDATE_ROBLOX_COOKIE", "0"},
      {"BLOX7_ENGINE_DETACH", "0"},
      {"BLOX7_CALL_REAL_APP_BRIDGE_INIT", "1"},
      {"BLOX7_CALL_REAL_APP_BRIDGE_INIT_THREAD", "0"},
      {"BLOX7_SKIP_LIBROBLOX_CTORS", "0"},
      {"BLOX7_HOST_JNI_SINGLETON_SEED", "0"},
      {"BLOX7_INIT_CLIENT_SETTINGS", "1"},
      {"BLOX7_HEADLESS", "0"},
      {"BLOX7_STARTUP_THREAD_TIMEOUT_MS", "120000"},
      {"BLOX7_CALL_REAL_APP_BRIDGE_START", "1"},
      {"BLOX7_STEP_START_APP_WITH_PARAMS", "1"},
      {"BLOX7_UPDATE_SURFACE_APP_AFTER_START_APP", "1"},
      {"BLOX7_CALL_REAL_APP_BRIDGE_UPDATE_SURFACE", "1"},
      {"BLOX7_START_LUA_APP_DM", "1"},
      {"BLOX7_STEP_START_LUA_APP_DM", "1"},
      {"BLOX7_POST_CLIENT_SETTINGS", "1"},
      {"BLOX7_STEP_POST_CLIENT_SETTINGS", "1"},
      {"BLOX7_FETCH_CLIENT_SETTINGS", "1"},
      {"BLOX7_START_LUA_APP_DM_AFTER_START_APP", "0"},
      {"BLOX7_START_LUA_APP_DM_INLINE", "1"},
      {"BLOX7_START_LUA_APP_DM_DELAY_MS", "0"},
      {"BLOX7_START_GAME_WITH_PARAM", "0"},
      {"BLOX7_RESUME_GAME_WITH_PLATFORM_PARAMS_AFTER_START_GAME", "0"},
      {"BLOX7_SEND_APP_READY", "0"},
      {"BLOX7_SEND_GAME_LOADED", "0"},
      {"BLOX7_AUTO_EXIT_AFTER_PRESENT_MS", "0"},
  };
  for (const auto& [name, value] : defaults) {
    if (!SetDefault(name, value, error)) {
      return false;
    }
  }
  return true;
}

}  // namespace runtime
}  // namespace blox7

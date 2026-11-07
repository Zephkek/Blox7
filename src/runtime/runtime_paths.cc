#include "runtime/runtime_paths.h"

#define JSON_NOEXCEPTION 1
#include <array>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

#include "compat/guest_abi.h"

namespace blox7 {
namespace runtime {
namespace {

bool LegacyEnabled(const Environment& environment, std::string_view name) {
  const std::optional<std::string> value = environment.Get(name);
  return value.has_value() && !value->empty() && *value != "0";
}

std::filesystem::path XdgHome(const Environment& environment,
                              std::string_view variable,
                              const std::filesystem::path& fallback) {
  const std::filesystem::path configured = environment.GetOr(variable, "");
  return configured.is_absolute() ? configured : fallback;
}

bool IsLowerHexBuildId(std::string_view value) {
  if (value.size() != 40) {
    return false;
  }
  for (const unsigned char character : value) {
    if (!std::isdigit(character) && !(character >= 'a' && character <= 'f')) {
      return false;
    }
  }
  return true;
}

std::optional<std::string> ApprovalGeneration(
    const std::filesystem::path& relative, std::string_view directory_name,
    std::string_view payload_id, std::string* error) {
  const std::filesystem::path expected_directory = std::string(directory_name);
  const std::string filename = relative.filename().string();
  const std::string prefix = std::string(payload_id) + "-";
  constexpr std::string_view kJsonSuffix = ".json";
  if (relative.is_absolute() || relative != relative.lexically_normal() ||
      relative.parent_path() != expected_directory ||
      filename.size() != prefix.size() + 40 + kJsonSuffix.size() ||
      filename.compare(0, prefix.size(), prefix) != 0 ||
      filename.compare(filename.size() - kJsonSuffix.size(), kJsonSuffix.size(),
                       kJsonSuffix) != 0) {
    if (error != nullptr) {
      *error = "active payload approval path is not a generated direct " +
               std::string(directory_name) + " child";
    }
    return std::nullopt;
  }
  std::string generation = filename.substr(prefix.size(), 40);
  if (!IsLowerHexBuildId(generation)) {
    if (error != nullptr) {
      *error = "active payload approval generation is invalid";
    }
    return std::nullopt;
  }
  return generation;
}

bool ResolveApprovalFile(const std::filesystem::path& data_root,
                         const std::filesystem::path& relative,
                         std::string_view directory_name,
                         std::string_view payload_id,
                         std::string_view generation,
                         std::filesystem::path* resolved, std::string* error) {
  const std::filesystem::path expected_filename =
      std::string(payload_id) + "-" + std::string(generation) + ".json";
  const std::filesystem::path expected_directory = std::string(directory_name);
  if (relative.is_absolute() || relative != relative.lexically_normal() ||
      relative.parent_path() != expected_directory ||
      relative.filename() != expected_filename) {
    if (error != nullptr) {
      *error = "active payload approval path is not a direct " +
               std::string(directory_name) + " child";
    }
    return false;
  }

  std::error_code filesystem_error;
  const std::filesystem::path candidate = data_root / relative;
  const std::filesystem::file_status status =
      std::filesystem::symlink_status(candidate, filesystem_error);
  if (filesystem_error || !std::filesystem::is_regular_file(status) ||
      std::filesystem::is_symlink(status)) {
    if (error != nullptr) {
      *error = "active payload approval file is missing, invalid, or a symlink";
    }
    return false;
  }
  const std::filesystem::path approval_directory =
      data_root / expected_directory;
  const std::filesystem::file_status directory_status =
      std::filesystem::symlink_status(approval_directory, filesystem_error);
  if (filesystem_error || !std::filesystem::is_directory(directory_status) ||
      std::filesystem::is_symlink(directory_status)) {
    if (error != nullptr) {
      *error =
          "active payload approval directory is missing, invalid, or a symlink";
    }
    return false;
  }
  const std::filesystem::path canonical_data_root =
      std::filesystem::canonical(data_root, filesystem_error);
  if (filesystem_error) {
    if (error != nullptr) {
      *error = "cannot canonicalize active payload data root";
    }
    return false;
  }
  const std::filesystem::path canonical_directory =
      std::filesystem::canonical(approval_directory, filesystem_error);
  if (filesystem_error ||
      canonical_directory.parent_path() != canonical_data_root) {
    if (error != nullptr) {
      *error = "active payload approval directory escapes the data root";
    }
    return false;
  }
  const std::filesystem::path canonical_candidate =
      std::filesystem::canonical(candidate, filesystem_error);
  if (filesystem_error ||
      canonical_candidate.parent_path() != canonical_directory) {
    if (error != nullptr) {
      *error = "active payload approval file escapes its storage directory";
    }
    return false;
  }
  if (resolved != nullptr) {
    *resolved = canonical_candidate;
  }
  return true;
}

bool SetEnvironmentDefault(const char* name,
                           const std::filesystem::path& value,
                           std::string* error) {
  const char* existing = std::getenv(name);
  if (existing != nullptr && existing[0] != '\0') {
    return true;
  }
  if (setenv(name, value.c_str(), 1) == 0) {
    return true;
  }
  if (error != nullptr) {
    *error = std::string("cannot export resolved runtime path: ") + name;
  }
  return false;
}

struct ManagedPayloadBinding {
  std::filesystem::path target;
  std::filesystem::path link;
  const char* name;
  bool directory;
};

bool ResolveManagedPayloadBinding(ManagedPayloadBinding* binding,
                                  std::string* error) {
  std::error_code filesystem_error;
  const std::filesystem::file_status target_status =
      std::filesystem::symlink_status(binding->target, filesystem_error);
  const bool expected_type =
      binding->directory ? std::filesystem::is_directory(target_status)
                         : std::filesystem::is_regular_file(target_status);
  if (filesystem_error || !expected_type ||
      std::filesystem::is_symlink(target_status)) {
    if (error != nullptr) {
      *error = "managed payload " + std::string(binding->name) +
               " target is missing, invalid, or a symlink: " +
               binding->target.string();
    }
    return false;
  }
  binding->target =
      std::filesystem::canonical(binding->target, filesystem_error);
  if (filesystem_error) {
    if (error != nullptr) {
      *error = "cannot canonicalize managed payload " +
               std::string(binding->name) + " target: " +
               binding->target.string();
    }
    return false;
  }

  const std::filesystem::file_status link_status =
      std::filesystem::symlink_status(binding->link, filesystem_error);
  if (filesystem_error == std::errc::no_such_file_or_directory) {
    filesystem_error.clear();
    return true;
  }
  if (!filesystem_error &&
      link_status.type() == std::filesystem::file_type::not_found) {
    return true;
  }
  if (filesystem_error) {
    if (error != nullptr) {
      *error = "cannot inspect managed payload " +
               std::string(binding->name) + " path: " +
               binding->link.string();
    }
    return false;
  }
  if (!std::filesystem::is_symlink(link_status)) {
    if (error != nullptr) {
      *error = "refusing to replace non-symlink managed payload " +
               std::string(binding->name) + " path: " +
               binding->link.string();
    }
    return false;
  }
  return true;
}

bool PublishManagedPayloadBinding(const ManagedPayloadBinding& binding,
                                  std::string* error) {
  std::error_code filesystem_error;
  const std::filesystem::path current_target =
      std::filesystem::read_symlink(binding.link, filesystem_error);
  if (!filesystem_error && current_target == binding.target) {
    return true;
  }
  if (filesystem_error == std::errc::no_such_file_or_directory) {
    filesystem_error.clear();
  } else if (filesystem_error) {
    if (error != nullptr) {
      *error = "cannot read managed payload " + std::string(binding.name) +
               " link: " + binding.link.string();
    }
    return false;
  }

  static std::atomic_uint64_t sequence{0};
  const std::filesystem::path temporary_link =
      binding.link.string() + ".blox7-next-" +
      std::to_string(static_cast<unsigned long long>(getpid())) + "-" +
      std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
  const std::filesystem::file_status temporary_status =
      std::filesystem::symlink_status(temporary_link, filesystem_error);
  if (filesystem_error == std::errc::no_such_file_or_directory ||
      (!filesystem_error && temporary_status.type() ==
                                std::filesystem::file_type::not_found)) {
    filesystem_error.clear();
  } else {
    if (error != nullptr) {
      *error = "managed payload temporary link already exists: " +
               temporary_link.string();
    }
    return false;
  }

  if (binding.directory) {
    std::filesystem::create_directory_symlink(binding.target, temporary_link,
                                              filesystem_error);
  } else {
    std::filesystem::create_symlink(binding.target, temporary_link,
                                    filesystem_error);
  }
  if (!filesystem_error) {
    std::filesystem::rename(temporary_link, binding.link, filesystem_error);
  }
  if (filesystem_error) {
    std::error_code cleanup_error;
    std::filesystem::remove(temporary_link, cleanup_error);
    if (error != nullptr) {
      *error = "cannot publish managed payload " + std::string(binding.name) +
               " link: " + binding.link.string();
    }
    return false;
  }
  return true;
}

}  // namespace

// we used to have another name. moving day, once, so nobody gets signed out
constexpr char kOldName[] = "mock" "tail";

static std::filesystem::path MoveOldFolder(const std::filesystem::path& base, bool* moved = nullptr) {
  std::error_code error;
  const std::filesystem::path old = base / kOldName, fresh = base / "blox7";
  if (std::filesystem::is_directory(old, error) && !std::filesystem::exists(fresh, error)) {
    std::filesystem::rename(old, fresh, error);
    if (moved != nullptr) *moved = !error;
  }
  return fresh;
}

static void RenameOldFiles(const std::filesystem::path& dir) {
  std::error_code error;
  std::vector<std::filesystem::path> old;
  for (const auto& entry : std::filesystem::directory_iterator(dir, error))
    if (entry.path().filename().string().find(kOldName) != std::string::npos) old.push_back(entry.path());
  for (const auto& path : old) {
    std::string name = path.filename().string();
    name.replace(name.find(kOldName), sizeof kOldName - 1, "blox7");
    std::filesystem::rename(path, dir / name, error);
  }
}

// approvals point at libroblox by full path, so they move too
static void FixMovedData(const std::filesystem::path& data_root) {
  std::error_code error;
  const std::string before = std::string("/") + kOldName + "/payloads/", after = "/blox7/payloads/";
  for (const auto& entry : std::filesystem::directory_iterator(data_root / "approvals", error)) {
    if (entry.path().extension() != ".json") continue;
    std::ifstream in(entry.path(), std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    bool moved = false;
    for (std::size_t at; (at = text.find(before)) != std::string::npos; moved = true)
      text.replace(at, before.size(), after);
    if (!moved) continue;
    const std::filesystem::path temporary = entry.path().string() + ".tmp";
    std::ofstream(temporary, std::ios::binary | std::ios::trunc) << text;
    // evidence has to stay read-only or the loader throws it out
    std::filesystem::permissions(temporary, std::filesystem::perms::owner_read | std::filesystem::perms::group_read |
                                                std::filesystem::perms::others_read, error);
    std::filesystem::rename(temporary, entry.path(), error);
  }
  for (const auto& dir : {data_root, data_root / "auth", data_root / "android/data"}) RenameOldFiles(dir);
}

RuntimePaths RuntimePaths::FromEnvironment(
    const Environment& environment, std::filesystem::path working_directory) {
  RuntimePaths paths;
  paths.home_ = environment.GetOr("HOME", "/root");
  if (working_directory.empty()) {
    std::error_code error;
    working_directory = std::filesystem::current_path(error);
    if (error) {
      working_directory = ".";
    }
  }
  paths.working_directory_ = std::move(working_directory);

  paths.sober_data_root_ =
      paths.home_ / ".var/app/org.vinegarhq.Sober/data/sober";
  paths.sober_cache_root_ =
      paths.home_ / ".var/app/org.vinegarhq.Sober/cache/sober";
  if (environment.HasNonEmpty("BLOX7_CONFIG_ROOT")) {
    paths.config_root_ = environment.GetOr("BLOX7_CONFIG_ROOT", "");
  } else {
    paths.config_root_ =
        MoveOldFolder(XdgHome(environment, "XDG_CONFIG_HOME", paths.home_ / ".config"));
  }

  bool data_moved = false, state_moved = false;
  const std::filesystem::path xdg_data_home =
      XdgHome(environment, "XDG_DATA_HOME", paths.home_ / ".local/share");
  const std::filesystem::path xdg_cache_home =
      XdgHome(environment, "XDG_CACHE_HOME", paths.home_ / ".cache");
  const std::filesystem::path xdg_state_home =
      XdgHome(environment, "XDG_STATE_HOME", paths.home_ / ".local/state");
  paths.data_root_ =
      environment.HasNonEmpty("BLOX7_DATA_ROOT")
          ? std::filesystem::path(environment.GetOr("BLOX7_DATA_ROOT", ""))
          : MoveOldFolder(xdg_data_home, &data_moved);
  paths.cache_root_ =
      environment.HasNonEmpty("BLOX7_CACHE_ROOT")
          ? std::filesystem::path(environment.GetOr("BLOX7_CACHE_ROOT", ""))
          : MoveOldFolder(xdg_cache_home);
  paths.state_root_ =
      environment.HasNonEmpty("BLOX7_STATE_ROOT")
          ? std::filesystem::path(environment.GetOr("BLOX7_STATE_ROOT", ""))
          : MoveOldFolder(xdg_state_home, &state_moved);
  paths.auth_root_ =
      environment.HasNonEmpty("BLOX7_AUTH_ROOT")
          ? std::filesystem::path(environment.GetOr("BLOX7_AUTH_ROOT", ""))
          : paths.data_root_ / "auth";
  if (data_moved) FixMovedData(paths.data_root_);
  if (state_moved) RenameOldFiles(paths.state_root_ / "support");
  paths.payloads_root_ = paths.data_root_ / "payloads";
  paths.downloads_root_ = paths.cache_root_ / "downloads";
  paths.logs_root_ = paths.state_root_ / "logs";
  paths.android_runtime_root_ = paths.data_root_ / "android";
  paths.android_data_root_ = paths.android_runtime_root_ / "data";
  paths.android_cache_root_ = paths.cache_root_ / "android";
  paths.vulkan_shader_cache_file_ =
      paths.cache_root_ / "graphics/shadercachevk.bin";
  paths.config_file_ = paths.config_root_ / "config.yaml";
  paths.active_payload_manifest_ = paths.data_root_ / "current.json";

  paths.cookie_file_ = paths.auth_root_ / "roblox.cookie";
  paths.use_real_sober_paths_ =
      LegacyEnabled(environment, "BLOX7_USE_REAL_SOBER_PATHS");
  return paths;
}

std::filesystem::path RuntimePaths::DefaultAssetPath() const {
  const ActivePayloadPaths active = ResolveActivePayload();
  if (active && active.active) {
    return active.assets_content;
  }
  std::filesystem::path relative = "rbx_bin/assets/content";
  if (Exists(working_directory_ / relative)) {
    return relative;
  }
  return DefaultSoberAwarePath(sober_data_root_ / "assets/content", relative);
}

ActivePayloadPaths RuntimePaths::ResolveActivePayload() const {
  ActivePayloadPaths result;
  std::error_code filesystem_error;
  const std::filesystem::file_status manifest_status =
      std::filesystem::symlink_status(active_payload_manifest_,
                                      filesystem_error);
  if (filesystem_error == std::errc::no_such_file_or_directory) {
    return result;
  }
  if (filesystem_error || !std::filesystem::is_regular_file(manifest_status) ||
      std::filesystem::is_symlink(manifest_status)) {
    result.error = "active payload manifest is invalid or a symlink";
    return result;
  }

  std::ifstream input(active_payload_manifest_);
  const nlohmann::json manifest =
      nlohmann::json::parse(input, nullptr, false, true);
  if (manifest.is_discarded() || !manifest.is_object()) {
    result.error = "active payload manifest is not valid JSON";
    return result;
  }
  const auto schema = manifest.find("schema_version");
  const auto payload_id = manifest.find("payload_id");
  const auto payload_path = manifest.find("payload_path");
  const auto version_name = manifest.find("version_name");
  const auto version_code = manifest.find("version_code");
  const auto build_id = manifest.find("elf_build_id");
  if (schema == manifest.end() || !schema->is_number_integer() ||
      schema->get<int>() != 1 || payload_id == manifest.end() ||
      !payload_id->is_string() || payload_path == manifest.end() ||
      !payload_path->is_string() || version_name == manifest.end() ||
      !version_name->is_string() || version_name->get<std::string>().empty() ||
      version_code == manifest.end() || !version_code->is_number_integer() ||
      version_code->get<std::int64_t>() < 0 || build_id == manifest.end() ||
      !build_id->is_string()) {
    result.error = "active payload manifest has an unsupported schema";
    return result;
  }

  const std::filesystem::path relative = payload_path->get<std::string>();
  const std::string id = payload_id->get<std::string>();
  const std::string elf_build_id = build_id->get<std::string>();
  const std::string expected_id =
      std::to_string(version_code->get<std::int64_t>()) + "-" + elf_build_id;
  if (relative.is_absolute() || relative != relative.lexically_normal() ||
      relative.parent_path() != "payloads" || relative.filename() != id ||
      id != expected_id || !IsLowerHexBuildId(elf_build_id)) {
    result.error = "active payload path is not a direct payloads child";
    return result;
  }

  const std::filesystem::path root = data_root_ / relative;
  const std::filesystem::file_status root_status =
      std::filesystem::symlink_status(root, filesystem_error);
  if (filesystem_error || !std::filesystem::is_directory(root_status) ||
      std::filesystem::is_symlink(root_status)) {
    result.error = "active payload root is missing, invalid, or a symlink";
    return result;
  }
  const std::filesystem::path canonical_payloads =
      std::filesystem::weakly_canonical(payloads_root_, filesystem_error);
  if (filesystem_error) {
    result.error = "cannot canonicalize payload storage";
    return result;
  }
  const std::filesystem::path canonical_root =
      std::filesystem::canonical(root, filesystem_error);
  if (filesystem_error || canonical_root.parent_path() != canonical_payloads) {
    result.error = "active payload escapes payload storage";
    return result;
  }

  const std::filesystem::path library = canonical_root / "libroblox.so";
  const std::filesystem::path package_root = canonical_root / "sober_apk";
  const std::filesystem::path base_apk = package_root / "base.apk";
  const std::filesystem::path arch_split_apk =
      package_root / compat::kGuestSplitApkFile;
  const std::filesystem::path assets_root = canonical_root / "assets";
  const std::filesystem::path assets = canonical_root / "assets/content";
  const std::filesystem::file_status library_status =
      std::filesystem::symlink_status(library, filesystem_error);
  if (filesystem_error || !std::filesystem::is_regular_file(library_status) ||
      std::filesystem::is_symlink(library_status)) {
    result.error = "active payload libroblox.so is missing or invalid";
    return result;
  }
  const std::filesystem::file_status package_root_status =
      std::filesystem::symlink_status(package_root, filesystem_error);
  if (filesystem_error ||
      !std::filesystem::is_directory(package_root_status) ||
      std::filesystem::is_symlink(package_root_status)) {
    result.error = "active payload sober_apk root is missing or invalid";
    return result;
  }
  for (const auto& [path, name] :
       std::array<std::pair<std::filesystem::path, const char*>, 2>{
           std::pair{base_apk, "base.apk"},
           std::pair{arch_split_apk,
                     compat::kGuestSplitApkFile.data()}}) {
    const std::filesystem::file_status status =
        std::filesystem::symlink_status(path, filesystem_error);
    if (filesystem_error || !std::filesystem::is_regular_file(status) ||
        std::filesystem::is_symlink(status)) {
      result.error = "active payload " + std::string(name) +
                     " is missing or invalid";
      return result;
    }
  }
  const std::filesystem::file_status assets_root_status =
      std::filesystem::symlink_status(assets_root, filesystem_error);
  if (filesystem_error || !std::filesystem::is_directory(assets_root_status) ||
      std::filesystem::is_symlink(assets_root_status)) {
    result.error = "active payload assets root is missing or invalid";
    return result;
  }
  const std::filesystem::file_status assets_status =
      std::filesystem::symlink_status(assets, filesystem_error);
  if (filesystem_error || !std::filesystem::is_directory(assets_status) ||
      std::filesystem::is_symlink(assets_status)) {
    result.error = "active payload assets/content is missing or invalid";
    return result;
  }

  result.active = true;
  result.root = canonical_root;
  result.roblox_library = library;
  result.base_apk = base_apk;
  result.arch_split_apk = arch_split_apk;
  result.assets_content = assets;

  const auto compatibility_manifest =
      manifest.find("compatibility_manifest_path");
  const auto host_abi_profile = manifest.find("host_abi_profile_path");
  const auto approval_receipt = manifest.find("approval_path");
  const bool has_compatibility_manifest =
      compatibility_manifest != manifest.end();
  const bool has_host_abi_profile = host_abi_profile != manifest.end();
  const bool has_approval_receipt = approval_receipt != manifest.end();
  if (has_compatibility_manifest || has_host_abi_profile ||
      has_approval_receipt) {
    if (!has_compatibility_manifest || !has_host_abi_profile ||
        !has_approval_receipt || !compatibility_manifest->is_string() ||
        !host_abi_profile->is_string() || !approval_receipt->is_string()) {
      result.error =
          "active payload approval references must be a complete string set";
      result.active = false;
      return result;
    }
    const std::filesystem::path compatibility_path =
        compatibility_manifest->get<std::string>();
    const std::filesystem::path profile_path =
        host_abi_profile->get<std::string>();
    const std::filesystem::path receipt_path =
        approval_receipt->get<std::string>();
    const std::optional<std::string> compatibility_generation =
        ApprovalGeneration(compatibility_path, "compatibility_profiles", id,
                           &result.error);
    const std::optional<std::string> profile_generation = ApprovalGeneration(
        profile_path, "host_abi_profiles", id, &result.error);
    const std::optional<std::string> receipt_generation =
        ApprovalGeneration(receipt_path, "approvals", id, &result.error);
    if (!compatibility_generation.has_value() ||
        !profile_generation.has_value() || !receipt_generation.has_value() ||
        *compatibility_generation != *profile_generation ||
        *compatibility_generation != *receipt_generation) {
      if (result.error.empty()) {
        result.error =
            "active payload approval references use different generations";
      }
      result.active = false;
      return result;
    }
    if (!ResolveApprovalFile(data_root_, compatibility_path,
                             "compatibility_profiles", id,
                             *compatibility_generation,
                             &result.compatibility_manifest, &result.error) ||
        !ResolveApprovalFile(data_root_, profile_path, "host_abi_profiles", id,
                             *profile_generation, &result.host_abi_profile,
                             &result.error) ||
        !ResolveApprovalFile(
            data_root_, receipt_path, "approvals", id, *receipt_generation,
            &result.host_abi_approval_receipt, &result.error)) {
      result.active = false;
      return result;
    }
  }
  return result;
}

std::filesystem::path RuntimePaths::DefaultSoberAwarePath(
    const std::filesystem::path& sober_path,
    const std::filesystem::path& fallback_path) const {
  if (use_real_sober_paths_ && Exists(sober_path)) {
    return sober_path;
  }
  return fallback_path;
}

bool RuntimePaths::Exists(const std::filesystem::path& path) {
  if (path.empty()) {
    return false;
  }
  std::error_code error;
  return std::filesystem::exists(path, error) && !error;
}

bool RuntimePaths::EnsureDirectory(const std::filesystem::path& path,
                                   std::error_code* error) {
  std::error_code local_error;
  if (path.empty()) {
    local_error = std::make_error_code(std::errc::invalid_argument);
  } else if (!std::filesystem::create_directories(path, local_error) &&
             !local_error) {
    if (!std::filesystem::is_directory(path, local_error) && !local_error) {
      local_error = std::make_error_code(std::errc::not_a_directory);
    }
  }
  if (error != nullptr) {
    *error = local_error;
  }
  return !local_error;
}

bool ExportRuntimePathEnvironment(const RuntimePaths& paths,
                                  std::string* error) {
  std::error_code filesystem_error;
  for (const std::filesystem::path& directory :
       {paths.android_runtime_root(), paths.android_cache_root(),
        paths.vulkan_shader_cache_file().parent_path()}) {
    if (!RuntimePaths::EnsureDirectory(directory, &filesystem_error)) {
      if (error != nullptr) {
        *error = "cannot create resolved runtime path: " + directory.string();
      }
      return false;
    }
  }

  return SetEnvironmentDefault("BLOX7_RUNTIME_ROOT",
                               paths.android_runtime_root(), error) &&
         SetEnvironmentDefault("BLOX7_ANDROID_CACHE_HOST_ROOT",
                               paths.android_cache_root(), error) &&
         SetEnvironmentDefault("BLOX7_VULKAN_SHADER_CACHE_HOST_PATH",
                               paths.vulkan_shader_cache_file(), error) &&
         SetEnvironmentDefault("BLOX7_TEXTURE_OVERRIDE_DIR",
                               paths.config_root() / "textures", error);
}

bool PrepareManagedPayloadWorkingDirectory(const RuntimePaths& paths,
                                           const ActivePayloadPaths& active,
                                           std::string* error) {
  if (!active.active) {
    return true;
  }
  std::error_code filesystem_error;
  const std::filesystem::path rbx_root = paths.data_root() / "rbx_bin";
  if (!RuntimePaths::EnsureDirectory(rbx_root, &filesystem_error)) {
    if (error != nullptr) {
      *error = "cannot prepare managed payload working directory: " +
               rbx_root.string();
    }
    return false;
  }
  const std::filesystem::file_status rbx_root_status =
      std::filesystem::symlink_status(rbx_root, filesystem_error);
  if (filesystem_error || !std::filesystem::is_directory(rbx_root_status) ||
      std::filesystem::is_symlink(rbx_root_status)) {
    if (error != nullptr) {
      *error = "managed payload rbx_bin root is invalid or a symlink: " +
               rbx_root.string();
    }
    return false;
  }

  std::array<ManagedPayloadBinding, 3> bindings{{
      {active.root / "assets", rbx_root / "assets", "assets", true},
      {active.root / "sober_apk", rbx_root / "sober_apk", "APK directory",
       true},
      {active.root / "libroblox.so", rbx_root / "libroblox.so",
       "native library", false},
  }};
  for (ManagedPayloadBinding& binding : bindings) {
    if (!ResolveManagedPayloadBinding(&binding, error)) {
      return false;
    }
  }
  for (const ManagedPayloadBinding& binding : bindings) {
    if (!PublishManagedPayloadBinding(binding, error)) {
      return false;
    }
  }

  std::filesystem::current_path(paths.data_root(), filesystem_error);
  if (filesystem_error) {
    if (error != nullptr) {
      *error = "cannot activate managed payload working directory: " +
               paths.data_root().string();
    }
    return false;
  }
  return true;
}

std::filesystem::path ResolveAdjacentRobloxAssetPath(
    const std::filesystem::path& roblox_library,
    const std::filesystem::path& working_directory) {
  if (roblox_library.empty() || working_directory.empty()) {
    return {};
  }
  std::filesystem::path resolved_library = roblox_library;
  if (!resolved_library.is_absolute()) {
    resolved_library = working_directory / resolved_library;
  }
  resolved_library = resolved_library.lexically_normal();
  if (!resolved_library.is_absolute() || !resolved_library.has_parent_path()) {
    return {};
  }
  return (resolved_library.parent_path() / "assets/content").lexically_normal();
}

}  // namespace runtime
}  // namespace blox7

#include "runtime/runtime_config_bootstrap.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace blox7 {
namespace runtime {
namespace {

constexpr std::string_view kDefaultRuntimeConfig =
    R"yaml(# Blox7 configuration.
# The file is created once with mode 0600. Blox7 never replaces your edits.

# Integer: configuration schema version. Only version 1 is supported.
version: 1

# One-line device preset. Short aliases pc, mobile, and console are accepted.
# Presets: mobile-s22-ultra, mobile-pixel-7, pc-windows-11, console-ps5.
# Use the mapping below for a custom profile.
device: mobile-s22-ultra

# Detailed alternative (replace the scalar device line above):
# device:
#   type: mobile
#   platform: Android
#   name: Google Pixel 9 Pro
#   manufacturer: Google
#   model: Pixel 9 Pro
#   brand: google
#   code: komodo
#   sku: pixel-9-pro
#   soc_model: Google Tensor G4
#   touch: true
#   mouse: false
#   keyboard: false

runtime:
  # Boolean (default: false): start without a visible SDL window.
  headless: false
  # String (optional): use a specific x86_64 libroblox.so instead of the
  # active verified payload.
  # roblox_library: /absolute/path/to/libroblox.so

appearance:
  # String (default: roblox): use Roblox's saved account theme. Supported
  # overrides: dark, light, or system (follow the desktop color scheme).
  theme: roblox

graphics:
  # String (default: direct-vulkan): graphics backend. Use opengl on hardware
  # without a working Vulkan driver; OpenGL ES 3.0 or newer is required.
  # Values: direct-vulkan, opengl.
  backend: direct-vulkan
  # Optional string or integer: Blox7 frame-rate override. Leave commented
  # (or use -1) to let Roblox Basic Settings own the cap. Supported overrides:
  # display, unlimited, or any positive integer; fixed values are forwarded to
  # DFIntTaskSchedulerTargetFps without a whitelist.
  # frame_rate_limit: -1
  # Optional presentation synchronization override: auto, on, or off.
  # vsync: off

performance:
  # Boolean (default: false): size Roblox scheduler/render queues from every
  # physical CPU core. A place's Lua/main thread can still remain serial.
  multithreaded_rendering: false
  # String (default: throughput): `throughput` makes every physical core
  # available to Roblox worker pools; `latency` preserves Roblox-managed pool
  # sizes and coalesces midphase work. Supported: auto, latency, throughput.
  physics_worker_mode: throughput
  # Integer MiB (default: 0): stop Blox7 once its memory use passes this many
  # MiB. 0 disables the cap.
  memory_limit_mb: 0

audio:
  # String (default: default): `default` follows the host system output.
  # To pin output, copy an exact SDL device name printed during startup.
  output_device: default
  # String (default: default): input device name, `id:<number>` printed during
  # startup, or `disabled`. IDs disambiguate duplicate names but may change
  # between boots; prefer the exact device name when it is unique.
  input_device: default

integrations:
  fleasion:
    # Boolean (default: false): trust Fleasion's CA without editing Roblox files.
    enabled: false
    # String: match Fleasion's routing mode: env or hosts.
    proxy_mode: env
    # Integer: Fleasion's local HTTP proxy port in env mode.
    proxy_port: 58443
    # Optional absolute public CA path. Default: $XDG_CONFIG_HOME/Fleasion/proxy_ca/ca.crt
    # or ~/.config/Fleasion/proxy_ca/ca.crt. Never select ca.key.
    # ca_certificate: /home/user/.config/Fleasion/proxy_ca/ca.crt
  discord_rpc:
    # Boolean (default: false): publish Blox7 activity to Discord Desktop.
    # This never signs in to Discord and never reads an account token.
    enabled: false
    # Boolean (default: true): show the current Roblox experience name.
    show_place_name: true
    # Boolean (default: true): show how long the current session has run.
    show_elapsed_time: true
    join:
      # Boolean (default: true): let friends open the current experience. When
      # Roblox provides a public server ID, the button targets that server.
      enabled: true
      # Boolean (default: true): never expose private or reserved joins.
      public_servers_only: true
      # button_label: Join Server
    # English defaults. Uncomment only the lines you want to customize.
    # text:
    #   browsing: Browsing experiences
    #   joining: Joining an experience
    #   playing: "{place_name}"
    #   state: Playing Roblox
    #   unknown_place: Unknown experience
    # Advanced: overrides the application bundled by the package maintainer.
    # application_id: 123456789012345678

window:
  # Integer (default: 1280): initial game area width, in pixels on Windows.
  width: 1280
  # Integer (default: 720): initial game area height, in pixels on Windows.
  height: 720
  # String (default: Roblox): window title.
  title: Roblox
  # Boolean (default: false): render at physical display-pixel density instead
  # of the logical desktop resolution. Enable only for sharper high-DPI output.
  high_dpi: false
  # String (default: auto): size of Roblox's UI. auto follows Windows' display
  # scale; a percentage from 100 to 400 overrides it. At 100 Roblox counts sizes
  # in real pixels.
  ui_scale: auto

network:
  # Optional fixed HTTP/SOCKS5 proxy. Uncomment host and port together.
  # The host must not contain a scheme such as http://.
  # proxy_scheme: http
  # String (optional, default: disabled): proxy host name or IP address.
  # proxy_host: 127.0.0.1
  # Integer (optional, default: disabled): proxy TCP port from 1 to 65535.
  # proxy_port: 8080
  # proxy_username: "user"
  # proxy_password: "password"
  # Absolute path (optional): custom PEM CA bundle. Blox7 reads this file
  # directly and does not replace it during Roblox payload updates.
  # ca_bundle: /home/user/.config/blox7/cacert.pem

updates:
  # Boolean (default: true): check the latest Roblox version, derive an exact
  # HostAbi profile, run two isolated canaries with the selected graphics
  # backend, and promote only on success. An existing current payload is
  # preserved when probation fails.
  automatic: true
  # String (default: apk-pure): direct x86_64 APK provider selection. Both
  # accepted values resolve the provider chain, which currently starts with
  # APKPure. Supported values: auto, apk-pure.
  source: apk-pure
  # Absolute path (optional): directory of Roblox's own APKs, offered alongside
  # APKPure; the newest version either one has is installed. A base APK with
  # its split_config.x86_64.apk (as Google Play installs them) or one APK that
  # carries the x86_64 library. Verified like any download.
  # apk_directory: /home/user/roblox-apks
  # String (default: SPDMTiahh/roblox-updates): GitHub repository whose
  # releases carry Roblox's Google Play APKs (base + x86_64 split), checked
  # after apk_directory and before APKPure. Verified like any download. An
  # empty string turns it off.
  # github_repository: SPDMTiahh/roblox-updates
  # Reserved for desktop update integrations. `blox7_updater` itself never
  # launches another process after changing the active payload.
  launch_after_update: false
)yaml";

std::string ErrorMessage(std::string_view action, int error_number) {
  return std::string(action) + ": " + std::strerror(error_number);
}

bool WriteAll(int file, std::string_view contents, std::string* error) {
  const char* next = contents.data();
  std::size_t remaining = contents.size();
  while (remaining != 0) {
    const ssize_t written = write(file, next, remaining);
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      *error = ErrorMessage("cannot write configuration", errno);
      return false;
    }
    next += written;
    remaining -= static_cast<std::size_t>(written);
  }
  return true;
}

bool WriteTemporaryConfig(const std::filesystem::path& parent,
                          const std::filesystem::path& config_file,
                          std::string_view contents,
                          std::vector<char>* temporary_path,
                          std::string* error) {
  const std::string pattern =
      (parent / ("." + config_file.filename().string() + ".tmp.XXXXXX"))
          .string();
  temporary_path->assign(pattern.begin(), pattern.end());
  temporary_path->push_back('\0');
  const int file = mkstemp(temporary_path->data());
  if (file < 0) {
    *error = ErrorMessage("cannot create temporary configuration", errno);
    return false;
  }
  const auto fail = [&](std::string message) {
    (void)close(file);
    (void)unlink(temporary_path->data());
    *error = std::move(message);
    return false;
  };
  if (fcntl(file, F_SETFD, FD_CLOEXEC) != 0 ||
      fchmod(file, S_IRUSR | S_IWUSR) != 0) {
    return fail(ErrorMessage("cannot secure temporary configuration", errno));
  }
  std::string write_error;
  if (!WriteAll(file, contents, &write_error)) {
    return fail(write_error);
  }
  if (fsync(file) != 0) {
    return fail(ErrorMessage("cannot synchronize configuration", errno));
  }
  if (close(file) != 0) {
    const int close_error = errno;
    (void)unlink(temporary_path->data());
    *error = ErrorMessage("cannot close configuration", close_error);
    return false;
  }
  return true;
}

struct OldSetting {
  std::string_view section;
  std::string_view key;
};

constexpr OldSetting kOldSettings[] = {
    {"auth", "use_libsecret"},
    {"performance", "gamemode"},
    {"network", "use_system_proxy"},
};

std::string_view WithoutCarriageReturn(std::string_view line) {
  return !line.empty() && line.back() == '\r' ? line.substr(0, line.size() - 1)
                                              : line;
}

bool IsTopLevel(std::string_view line) {
  return !line.empty() && line.front() != ' ' && line.front() != '#';
}

std::string_view Indented(std::string_view line, std::size_t* indent) {
  const std::size_t first = line.find_first_not_of(' ');
  *indent = first == std::string_view::npos ? line.size() : first;
  return line.substr(*indent);
}

std::string WithoutOldSettings(std::string_view text) {
  std::vector<std::string_view> lines;
  for (std::size_t begin = 0; begin <= text.size();) {
    const std::size_t end = text.find('\n', begin);
    if (end == std::string_view::npos) {
      lines.push_back(text.substr(begin));
      break;
    }
    lines.push_back(text.substr(begin, end - begin));
    begin = end + 1;
  }
  std::vector<bool> drop(lines.size(), false);
  std::vector<std::size_t> emptied_headers;
  std::string_view section;
  std::size_t header = 0;
  for (std::size_t i = 0; i < lines.size(); ++i) {
    const std::string_view line = WithoutCarriageReturn(lines[i]);
    if (IsTopLevel(line)) {
      section = line.substr(0, line.find(':'));
      header = i;
      continue;
    }
    std::size_t indent = 0;
    const std::string_view content = Indented(line, &indent);
    if (indent != 2 || content.empty() || content.front() == '#') continue;
    for (const OldSetting& removed : kOldSettings) {
      if (section != removed.section ||
          content.substr(0, removed.key.size()) != removed.key ||
          content.substr(removed.key.size(), 1) != ":") {
        continue;
      }
      drop[i] = true;
      for (std::size_t above = i; above > header + 1; --above) {
        std::size_t above_indent = 0;
        const std::string_view comment =
            Indented(WithoutCarriageReturn(lines[above - 1]), &above_indent);
        if (above_indent != 2 || comment.empty() || comment.front() != '#') {
          break;
        }
        drop[above - 1] = true;
      }
      emptied_headers.push_back(header);
    }
  }
  if (emptied_headers.empty()) return std::string(text);
  for (const std::size_t candidate : emptied_headers) {
    std::size_t end = candidate + 1;
    bool has_content = false;
    for (; end < lines.size(); ++end) {
      const std::string_view line = WithoutCarriageReturn(lines[end]);
      if (!line.empty() && line.front() != ' ') break;
      if (!drop[end] && !line.empty()) has_content = true;
    }
    if (has_content) continue;
    for (std::size_t i = candidate; i < end; ++i) {
      if (!WithoutCarriageReturn(lines[i]).empty()) drop[i] = true;
    }
  }
  std::string result;
  bool previous_blank = false;
  for (std::size_t i = 0; i < lines.size(); ++i) {
    if (drop[i]) continue;
    const bool blank = WithoutCarriageReturn(lines[i]).empty();
    if (blank && previous_blank && i + 1 < lines.size()) continue;
    previous_blank = blank;
    result.append(lines[i]);
    if (i + 1 < lines.size()) result.push_back('\n');
  }
  return result;
}

bool ReadConfig(const std::filesystem::path& config_file, std::string* text,
                std::string* error) {
  std::ifstream input(config_file, std::ios::binary);
  if (!input) {
    *error = "cannot read runtime configuration";
    return false;
  }
  std::ostringstream contents;
  contents << input.rdbuf();
  *text = contents.str();
  return true;
}

bool CleanOldSettings(const std::filesystem::path& config_file, bool* changed,
                       std::string* error) {
  std::string text;
  if (!ReadConfig(config_file, &text, error)) return false;
  const std::string cleaned = WithoutOldSettings(text);
  if (cleaned == text) return true;
  std::filesystem::path parent = config_file.parent_path();
  if (parent.empty()) parent = ".";
  std::vector<char> temporary_path;
  if (!WriteTemporaryConfig(parent, config_file, cleaned, &temporary_path,
                            error)) {
    return false;
  }
  if (rename(temporary_path.data(), config_file.c_str()) != 0) {
    const int rename_error = errno;
    (void)unlink(temporary_path.data());
    *error = ErrorMessage("cannot replace runtime configuration", rename_error);
    return false;
  }
  *changed = true;
  return true;
}

bool InspectExistingConfig(const std::filesystem::path& config_file,
                           bool* exists, std::string* error) {
  struct stat metadata = {};
  if (lstat(config_file.c_str(), &metadata) != 0) {
    if (errno == ENOENT) {
      *exists = false;
      return true;
    }
    *error = ErrorMessage("cannot inspect first-run configuration", errno);
    return false;
  }
  *exists = true;
  if (!S_ISREG(metadata.st_mode)) {
    *error = S_ISLNK(metadata.st_mode)
                 ? "refusing symlink runtime configuration"
                 : "runtime configuration must be a regular file";
    return false;
  }
  return true;
}

}  // namespace

std::string_view DefaultRuntimeConfigYaml() { return kDefaultRuntimeConfig; }

RuntimeConfigBootstrapResult EnsureRuntimeConfigFile(
    const std::filesystem::path& config_file) {
  RuntimeConfigBootstrapResult result;
  if (config_file.empty() || config_file.filename().empty()) {
    result.error = "first-run configuration path must name a file";
    return result;
  }

  bool exists = false;
  if (!InspectExistingConfig(config_file, &exists, &result.error)) {
    return result;
  }
  if (exists) {
    bool changed = false;
    if (CleanOldSettings(config_file, &changed, &result.error) && changed) {
      result.status = RuntimeConfigBootstrapStatus::kCleanedExisting;
    }
    return result;
  }

  std::filesystem::path parent = config_file.parent_path();
  if (parent.empty()) {
    parent = ".";
  }
  std::error_code filesystem_error;
  const bool parent_created =
      std::filesystem::create_directories(parent, filesystem_error);
  if (filesystem_error) {
    result.error = "cannot create configuration directory " + parent.string() +
                   ": " + filesystem_error.message();
    return result;
  }
  if (parent_created) {
    const int parent_directory =
        open(parent.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
    if (parent_directory < 0 || fchmod(parent_directory, S_IRWXU) != 0) {
      const int operation_error = errno;
      if (parent_directory >= 0) {
        (void)close(parent_directory);
      }
      result.error = ErrorMessage("cannot secure configuration directory",
                                  operation_error);
      return result;
    }
    if (close(parent_directory) != 0) {
      result.error =
          ErrorMessage("cannot close configuration directory", errno);
      return result;
    }
  }

  std::vector<char> temporary_path;
  if (!WriteTemporaryConfig(parent, config_file, kDefaultRuntimeConfig,
                            &temporary_path, &result.error)) {
    return result;
  }
  if (link(temporary_path.data(), config_file.c_str()) != 0) {
    const int publish_error = errno;
    (void)unlink(temporary_path.data());
    if (publish_error == EEXIST) {
      bool raced_entry_exists = false;
      if (!InspectExistingConfig(config_file, &raced_entry_exists,
                                 &result.error)) {
        return result;
      }
      if (!raced_entry_exists) {
        result.error = "runtime configuration disappeared during creation";
      }
      return result;
    }
    result.error =
        ErrorMessage("cannot publish first-run configuration", publish_error);
    return result;
  }
  result.status = RuntimeConfigBootstrapStatus::kCreated;
  if (unlink(temporary_path.data()) != 0) {
    result.error = ErrorMessage(
        "configuration was created but its temporary link cannot be removed",
        errno);
  }
  return result;
}

}  // namespace runtime
}  // namespace blox7

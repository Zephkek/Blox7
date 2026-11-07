#pragma once

#include <windows.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace blox7 {

struct Paths {
  std::wstring exe;
  std::wstring home;
  std::wstring runtime;
  std::wstring data;
  std::wstring webview2;
};

struct OsInfo {
  DWORD major = 0, minor = 0, build = 0;
  bool windows7() const { return major == 6 && minor == 1; }
};

struct UpdateState {
  std::wstring phase;
  std::wstring message;
  FILETIME written{};
};

struct Settings {
  std::wstring backend = L"direct-vulkan";
  std::wstring frame_rate_limit = L"-1";
  std::wstring vsync = L"auto";
  std::wstring present_mode = L"auto";
  std::wstring worker_mode = L"throughput";
  bool threaded_rendering = false;
  int width = 1280;
  int height = 720;
  bool fullscreen = false;
  bool high_dpi = false;
  int refresh_rate = 0;
  std::wstring effects = L"off";
  float sharpen = 0, vibrance = 0, contrast = 1, brightness = 0;
  bool fxaa = false;
  std::wstring shader;
  std::wstring device = L"mobile-s22-ultra";
  std::wstring quality = L"auto";
  std::wstring gpu;
  bool join_links = false;
  int render_scale = 100;
};

enum class DeviceKind { kPhone, kTablet, kPc, kConsole };

struct DeviceChoice {
  std::wstring id;
  std::wstring name;
  std::wstring detail;
  DeviceKind kind;
};

struct GpuChoice {
  std::wstring id;
  std::wstring name;
  std::wstring short_name;
  uint64_t memory = 0;
};

struct FastFlag {
  std::wstring name;
  std::wstring value;
  bool managed = false;
  std::wstring owner;
};

using Progress = std::function<void(double fraction)>;

Settings load_settings(const Paths& paths);
std::wstring save_settings(const Paths& paths, const Settings& settings);
bool vulkan_installed();
void apply_launch_settings(const Paths& paths, bool vulkan_driver = vulkan_installed());
void refresh_join_links(const Paths& paths);
std::vector<DeviceChoice> device_choices();
std::vector<GpuChoice> gpu_choices();
std::vector<int> refresh_rates();
std::vector<FastFlag> load_fast_flags(const Paths& paths);
std::wstring check_fast_flag(const std::wstring& name, std::wstring* value, const Settings& pending);
std::wstring save_fast_flags(const Paths& paths, const std::vector<FastFlag>& flags);
std::wstring refresh_shader(const Paths& paths, const std::wstring& source, bool* rebuilt);
std::wstring import_fast_flags(const std::wstring& file, std::vector<FastFlag>* flags, const Settings& pending, bool* failed);
SIZE render_size(const Settings& settings);
std::wstring export_fast_flags(const std::wstring& file, const std::vector<FastFlag>& flags);
void effects_preset(Settings* settings, const std::wstring& preset);
std::wstring compile_shader(const Paths& paths, const std::wstring& source);

Paths resolve_paths();
OsInfo os_info();
const wchar_t* payload_id();

bool runtime_ready(const Paths& paths);
std::wstring install_runtime(const Paths& paths, const Progress& progress);
void prepare_data(const Paths& paths);
void remove_old_runtimes(const Paths& paths);
void hold_runtime(const Paths& paths);
void set_runtime_environment(const Paths& paths);
std::string posix_path(const std::wstring& path);

std::wstring roblox_version(const Paths& paths);
std::wstring graphics_backend(const Paths& paths);
UpdateState update_state(const Paths& paths);
std::vector<std::wstring> log_tail(const Paths& paths, size_t lines);
std::wstring logs_dir(const Paths& paths);

std::wstring widen(const std::string& text);
std::string narrow(const std::wstring& text);
bool write_all(const std::wstring& path, const std::string& text);

int run_launcher(HINSTANCE instance, int show);
int render_ui(const std::wstring& dir);

}  // namespace blox7

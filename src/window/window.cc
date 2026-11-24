#include "window/window.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_video.h>
#include <SDL3/SDL_vulkan.h>
#include <dlfcn.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "blox7/graphics/gles_text_overlay_compositor.h"
#include "blox7/graphics/present_mode_policy.h"
#include "blox7/graphics/roblox_renderer_checks.h"
#include "blox7/platform/display_refresh_capabilities.h"
#include "blox7/platform/sdl_application_metadata.h"
#include "blox7/platform/sdl_event_converter.h"
#include "blox7/platform/sdl_window_icon.h"
#include "runtime/runtime_paths.h"
#include "window/input_pump_pacer.h"
#include "window/main_thread_command_gate.h"
#include "window/roblox_fullscreen_menu_request_gate.h"
#include "window/vulkan_present_progress_gate.h"
#include "window/vulkan_surface_recovery_gate.h"
#include "window/window_fullscreen_request_gate.h"
#include "window/window_fullscreen_state_sync.h"
#include "window/window_creation_policy.h"
#include "window/window_pointer_capture_owner.h"
#include "window/window_resize_readiness_gate.h"
#include "window/window_state_store.h"
#include "window/window_text_input_owner.h"

namespace blox7::win32 {
bool UpdateTextLayer(void* game_window);
void DestroyTextLayer();
}  // namespace blox7::win32
extern "C" void* __w32_pe_open(const char* utf8_path);
extern "C" void* __w32_pe_symbol(void* module, const char* name);
extern "C" const char* __w32_exe_dir(void);
extern "C" int blox7_angle_blob_cache(const char* dir);

namespace blox7 {
namespace window {

bool SetWindowsSwapInterval(int interval);

using EGLDisplay = void*;
using EGLConfig = void*;
using EGLContext = void*;
using EGLSurface = void*;
using EGLBoolean = unsigned int;
using EGLint = int;
using EGLenum = unsigned int;
using EGLNativeDisplayType = void*;
using EGLNativeWindowType = void*;
using __eglMustCastFP = void (*)();

static constexpr EGLint EGL_NONE_VAL = 0x3038;
static constexpr EGLint EGL_MIN_SWAP_INTERVAL_VAL = 0x303B;
static constexpr EGLint EGL_MAX_SWAP_INTERVAL_VAL = 0x303C;
static constexpr EGLint EGL_PLATFORM_ANGLE_ANGLE_VAL = 0x3202;
static constexpr EGLint EGL_PLATFORM_ANGLE_TYPE_ANGLE_VAL = 0x3203;
static constexpr EGLint EGL_PLATFORM_ANGLE_DEVICE_TYPE_ANGLE_VAL = 0x3209;
static constexpr EGLint EGL_PLATFORM_ANGLE_DEVICE_TYPE_HARDWARE_ANGLE_VAL =
    0x320a;
static constexpr EGLint EGL_PLATFORM_ANGLE_MAX_VERSION_MAJOR_ANGLE_VAL = 0x3204;
static constexpr EGLint EGL_PLATFORM_ANGLE_MAX_VERSION_MINOR_ANGLE_VAL = 0x3205;
static constexpr EGLint EGL_PLATFORM_ANGLE_TYPE_D3D11_ANGLE_VAL = 0x3208;
static constexpr EGLint EGL_PLATFORM_ANGLE_DEVICE_TYPE_D3D_WARP_ANGLE_VAL =
    0x320B;
static constexpr EGLint EGL_FEATURE_OVERRIDES_ENABLED_ANGLE_VAL = 0x3466;
static constexpr EGLint EGL_EXPERIMENTAL_PRESENT_PATH_ANGLE_VAL = 0x33A4;
static constexpr EGLint EGL_EXPERIMENTAL_PRESENT_PATH_FAST_ANGLE_VAL = 0x33A9;
static constexpr EGLBoolean EGL_TRUE_B = 1;
static constexpr EGLBoolean EGL_FALSE_B = 0;

using PFN_eglGetDisplay = EGLDisplay (*)(EGLNativeDisplayType);
using PFN_eglInitialize = EGLBoolean (*)(EGLDisplay, EGLint*, EGLint*);
using PFN_eglBindAPI = EGLBoolean (*)(EGLenum);
using PFN_eglChooseConfig = EGLBoolean (*)(EGLDisplay, const EGLint*,
                                           EGLConfig*, EGLint, EGLint*);
using PFN_eglCreateWindowSurface = EGLSurface (*)(EGLDisplay, EGLConfig,
                                                  EGLNativeWindowType,
                                                  const EGLint*);
using PFN_eglCreatePbufferSurface = EGLSurface (*)(EGLDisplay, EGLConfig,
                                                   const EGLint*);
using PFN_eglCreateContext = EGLContext (*)(EGLDisplay, EGLConfig, EGLContext,
                                            const EGLint*);
using PFN_eglMakeCurrent = EGLBoolean (*)(EGLDisplay, EGLSurface, EGLSurface,
                                          EGLContext);
using PFN_eglSwapBuffers = EGLBoolean (*)(EGLDisplay, EGLSurface);
using PFN_eglSwapInterval = EGLBoolean (*)(EGLDisplay, EGLint);
using PFN_eglGetConfigAttrib = EGLBoolean (*)(EGLDisplay, EGLConfig, EGLint,
                                              EGLint*);
using PFN_eglGetError = EGLint (*)();
using PFN_eglDestroyContext = EGLBoolean (*)(EGLDisplay, EGLContext);
using PFN_eglDestroySurface = EGLBoolean (*)(EGLDisplay, EGLSurface);
using PFN_eglTerminate = EGLBoolean (*)(EGLDisplay);
using PFN_eglGetProcAddress = __eglMustCastFP (*)(const char*);

struct WindowState {
  SDL_Window* sdl_window = nullptr;
  EGLDisplay egl_display = nullptr;
  EGLConfig egl_config = nullptr;
  EGLSurface egl_surface = nullptr;
  EGLContext egl_context = nullptr;
  PFN_eglSwapInterval egl_swap_interval = nullptr;
  int min_swap_interval = 1;
  int max_swap_interval = 1;
  void* native_window = nullptr;
  int width = 1280;
  int height = 720;
  bool initialised = false;
  bool quit_requested = false;
  bool visible = false;
  bool direct_vulkan = false;
  bool input_test_sequence_queued = false;
  bool fullscreen_readiness_requested = false;
  bool fullscreen_readiness_return_requested = false;
  bool restored_fullscreen_sync_pending = false;
  int fullscreen_readiness_present_baseline = 0;
  bool state_persistence_active = false;
  bool state_persistence_dirty = false;
  uint64_t state_persistence_change_ticks_ns = 0;
  PersistedWindowState persisted_window;
  platform::DisplayRefreshCapabilities display_refresh;
  int resize_target_logical_width = 0;
  int resize_target_logical_height = 0;
};

static WindowState g_state;
static std::atomic<int> g_real_swap_count{0};
static std::atomic<uint64_t> g_first_present_ticks_ns{0};
static std::atomic<bool> g_fullscreen_switching{false};
static std::atomic<SDL_ThreadID> g_pump_thread_id{0};
static std::atomic<bool> g_show_requested{false};
static void (*g_live_resize_tick)(void*) = nullptr;
static void* g_live_resize_context = nullptr;
static bool g_in_live_resize_tick = false;
static bool g_live_resize_watch_added = false;

static bool SDLCALL LiveResizeWatch(void*, SDL_Event* event) {
  if (event->type == SDL_EVENT_WINDOW_EXPOSED && event->window.data1 == 1 &&
      g_live_resize_tick != nullptr && !g_in_live_resize_tick) {
    g_in_live_resize_tick = true;
    g_live_resize_tick(g_live_resize_context);
    g_in_live_resize_tick = false;
  }
  return true;
}

void SetLiveResizeTick(void (*tick)(void*), void* context) {
  g_live_resize_tick = tick;
  g_live_resize_context = context;
  if (tick != nullptr && !g_live_resize_watch_added) {
    g_live_resize_watch_added = SDL_AddEventWatch(LiveResizeWatch, nullptr);
  }
}
static InputPumpPacer g_input_pump_pacer;
static PresentLifecycleGate g_present_lifecycle;
static VulkanPresentProgressGate g_vulkan_present_progress_gate;
static thread_local uint64_t g_vulkan_host_present_sequence = 0;
static VulkanSurfaceRecoveryGate g_vulkan_surface_recovery_gate;
static WindowSurfaceLifecycle g_window_surface_lifecycle;
static PlatformEventObserverGate g_platform_event_observer;
static MainThreadCommandGate g_pre_text_input_pump_gate;
static WindowFullscreenRequestGate g_fullscreen_request_gate;
static RobloxFullscreenMenuRequestGate g_fullscreen_menu_request_gate;
static WindowFullscreenStateSync g_fullscreen_state_sync;
static std::unique_ptr<WindowResizeReadinessGate> g_resize_readiness_gate;
static std::unique_ptr<SdlTextInputBackend> g_text_input_backend;
static std::unique_ptr<WindowTextInputOwner> g_text_input_owner;
static std::unique_ptr<SdlPointerCaptureBackend> g_pointer_capture_backend;
static std::unique_ptr<WindowPointerCaptureOwner> g_pointer_capture_owner;
static std::unique_ptr<graphics::GlesTextOverlayCompositor> g_gles_text_overlay;
static bool g_auto_angle_retry_attempted = false;
static bool g_gpu_lacks_gles3 = false;
static std::filesystem::path g_window_state_path;

WindowStartupPresentationPlan RestoredWindowPresentationPlan() {
  if (!g_state.state_persistence_active) {
    return {};
  }
  return PlanWindowStartupPresentation(g_state.persisted_window);
}

void ApplyMinimumWindowSize(SDL_Window* window) {
  if (window == nullptr) {
    return;
  }
  if (!SDL_SetWindowMinimumSize(window, kMinimumWindowWidth,
                                kMinimumWindowHeight)) {
    std::fprintf(stderr, "  [window] SDL minimum size rejected: %s\n",
                 SDL_GetError());
  }
}

void ApplyRestoredWindowMaximization(
    SDL_Window* window, const WindowStartupPresentationPlan& plan) {
  if (window == nullptr || !plan.maximize_after_constraints) {
    return;
  }
  if (!SDL_MaximizeWindow(window)) {
    std::fprintf(stderr, "  [window-state] SDL maximize restore failed: %s\n",
                 SDL_GetError());
  }
}

void ApplyWindowIcon(SDL_Window* window) {
  const Status status = platform::ApplySdlWindowIcon(window);
  if (!status.ok()) {
    std::fprintf(stderr, "  [window] SDL window icon unavailable: %s\n",
                 status.message().c_str());
  }
}

void RedrawWholeClientOnResize(SDL_Window* window) {
  using GetClassLongPtrFn = uintptr_t(__attribute__((ms_abi)) *)(void*, int32_t);
  using SetClassLongPtrFn =
      uintptr_t(__attribute__((ms_abi)) *)(void*, int32_t, uintptr_t);
  static void* const user32 = __w32_pe_open("user32.dll");
  static const auto get_class_long = reinterpret_cast<GetClassLongPtrFn>(
      user32 != nullptr ? __w32_pe_symbol(user32, "GetClassLongPtrW")
                        : nullptr);
  static const auto set_class_long = reinterpret_cast<SetClassLongPtrFn>(
      user32 != nullptr ? __w32_pe_symbol(user32, "SetClassLongPtrW")
                        : nullptr);
  void* hwnd =
      SDL_GetPointerProperty(SDL_GetWindowProperties(window),
                             SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
  if (get_class_long == nullptr || set_class_long == nullptr ||
      hwnd == nullptr) {
    return;
  }
  constexpr int32_t kGclStyle = -26;
  constexpr uintptr_t kCsVRedraw = 0x0001;
  constexpr uintptr_t kCsHRedraw = 0x0002;
  set_class_long(hwnd, kGclStyle,
                 get_class_long(hwnd, kGclStyle) | kCsVRedraw | kCsHRedraw);
}

// borderless just inherits the desktop rate, so ask for exclusive fullscreen
void ApplyFullscreenRefreshMode(SDL_Window* window) {
  const char* value = std::getenv("BLOX7_FULLSCREEN_REFRESH");
  const float refresh = value != nullptr ? std::strtof(value, nullptr) : 0.0f;
  if (window == nullptr || refresh <= 0.0f) {
    return;
  }
  const SDL_DisplayID display = SDL_GetDisplayForWindow(window);
  const SDL_DisplayMode* desktop = SDL_GetDesktopDisplayMode(display);
  SDL_DisplayMode mode{};
  const bool found =
      desktop != nullptr &&
      SDL_GetClosestFullscreenDisplayMode(display, desktop->w, desktop->h,
                                          refresh, false, &mode);
  if (found && mode.w == desktop->w && mode.h == desktop->h &&
      mode.refresh_rate_numerator == desktop->refresh_rate_numerator &&
      mode.refresh_rate_denominator == desktop->refresh_rate_denominator) {
    std::fprintf(stderr,
                 "  [fullscreen] desktop already runs %.2f Hz, "
                 "staying borderless\n",
                 desktop->refresh_rate);
    return;
  }
  if (!found || !SDL_SetWindowFullscreenMode(window, &mode)) {
    std::fprintf(stderr, "  [fullscreen] no %.0f Hz mode, staying borderless: %s\n",
                 refresh, SDL_GetError());
    return;
  }
  std::fprintf(stderr, "  [fullscreen] exclusive mode %dx%d at %.2f Hz\n",
               mode.w, mode.h, mode.refresh_rate);
}

bool TraceFlagEnabled(const char* name) {
  const char* value = std::getenv(name);
  return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
}

bool WindowTraceEnabled() {
  static const bool enabled = TraceFlagEnabled("BLOX7_WINDOW_TRACE") ||
                              TraceFlagEnabled("BLOX7_TRACE_ALL") ||
                              TraceFlagEnabled("BLOX7_FULL_TRACE");
  return enabled;
}

const char* GetEnvNonEmpty(const char* name) {
  if (name == nullptr) {
    return nullptr;
  }
  const char* raw = std::getenv(name);
  return raw != nullptr && raw[0] != '\0' ? raw : nullptr;
}

bool IsEnabledEnv(const char* name) {
  const char* value = GetEnvNonEmpty(name);
  return value != nullptr && std::strcmp(value, "0") != 0;
}

bool IsDisabledEnv(const char* name) {
  const char* value = GetEnvNonEmpty(name);
  return value != nullptr &&
         (std::strcmp(value, "0") == 0 || std::strcmp(value, "false") == 0 ||
          std::strcmp(value, "FALSE") == 0);
}

bool WindowStatePersistenceSuppressed() {
  return IsEnabledEnv("BLOX7_RESIZE_READINESS") ||
         IsEnabledEnv("BLOX7_FULLSCREEN_READINESS") ||
         IsEnabledEnv("BLOX7_INPUT_READINESS") ||
         IsEnabledEnv("BLOX7_AUTO_EXIT_AFTER_PRESENT_MS");
}

bool SavedPositionIntersectsDisplay(const PersistedWindowState& state) {
  if (!state.has_position) {
    return false;
  }
  int display_count = 0;
  SDL_DisplayID* displays = SDL_GetDisplays(&display_count);
  if (displays == nullptr || display_count <= 0) {
    SDL_free(displays);
    return false;
  }
  const SDL_Rect saved = {state.x, state.y, state.width, state.height};
  bool visible = false;
  for (int index = 0; index < display_count && !visible; ++index) {
    SDL_Rect bounds{};
    SDL_Rect intersection{};
    visible = SDL_GetDisplayBounds(displays[index], &bounds) &&
              SDL_GetRectIntersection(&saved, &bounds, &intersection) &&
              intersection.w >= 64 && intersection.h >= 64;
  }
  SDL_free(displays);
  return visible;
}

void ApplyRestoredWindowPosition() {
  if (g_state.sdl_window == nullptr || !g_state.persisted_window.has_position) {
    return;
  }
  if (!SavedPositionIntersectsDisplay(g_state.persisted_window)) {
    std::fprintf(stderr,
                 "  [window-state] saved position is outside active "
                 "displays; using compositor placement\n");
    return;
  }
  if (!SDL_SetWindowPosition(g_state.sdl_window, g_state.persisted_window.x,
                             g_state.persisted_window.y)) {
    std::fprintf(stderr, "  [window-state] SDL position restore failed: %s\n",
                 SDL_GetError());
  }
}

void CaptureWindowState() {
  if (!g_state.state_persistence_active || g_state.sdl_window == nullptr) {
    return;
  }
  const SDL_WindowFlags flags = SDL_GetWindowFlags(g_state.sdl_window);
  g_state.persisted_window.fullscreen = (flags & SDL_WINDOW_FULLSCREEN) != 0;
  g_state.persisted_window.maximized = (flags & SDL_WINDOW_MAXIMIZED) != 0;
  if (g_state.persisted_window.fullscreen ||
      g_state.persisted_window.maximized) {
    return;
  }
  int width = 0;
  int height = 0;
  if (SDL_GetWindowSize(g_state.sdl_window, &width, &height) && width >= 160 &&
      height >= 120) {
    g_state.persisted_window.width = width;
    g_state.persisted_window.height = height;
  }
  int x = 0;
  int y = 0;
  if (SDL_GetWindowPosition(g_state.sdl_window, &x, &y)) {
    g_state.persisted_window.x = x;
    g_state.persisted_window.y = y;
    g_state.persisted_window.has_position = true;
  }
}

bool PersistWindowState() {
  if (!g_state.state_persistence_active || g_window_state_path.empty()) {
    return true;
  }
  CaptureWindowState();
  const Status status =
      StoreWindowState(g_window_state_path, g_state.persisted_window);
  if (!status.ok()) {
    std::fprintf(stderr, "  [window-state] save failed: %s\n",
                 status.message().c_str());
    return false;
  }
  g_state.state_persistence_dirty = false;
  return true;
}

void MarkWindowStateDirty(bool flush_immediately) {
  if (!g_state.state_persistence_active) {
    return;
  }
  g_state.state_persistence_dirty = true;
  g_state.state_persistence_change_ticks_ns = SDL_GetTicksNS();
  if (flush_immediately) {
    (void)PersistWindowState();
  }
}

void MaybePersistWindowState() {
  constexpr uint64_t kSaveQuietPeriodNs = 250000000ULL;
  if (!g_state.state_persistence_dirty ||
      SDL_GetTicksNS() - g_state.state_persistence_change_ticks_ns <
          kSaveQuietPeriodNs) {
    return;
  }
  (void)PersistWindowState();
}

void MaybeSynchronizeRestoredFullscreenState() {
  if (!g_state.restored_fullscreen_sync_pending || !HasPresentedFrame()) {
    return;
  }
  const bool fullscreen =
      g_state.sdl_window != nullptr &&
      (SDL_GetWindowFlags(g_state.sdl_window) & SDL_WINDOW_FULLSCREEN) != 0;
  if (g_fullscreen_state_sync.Notify(fullscreen)) {
    g_state.restored_fullscreen_sync_pending = false;
  }
}

bool StringEquals(const char* value, const char* expected) {
  return value != nullptr && expected != nullptr &&
         std::strcmp(value, expected) == 0;
}

bool ParsePositiveInt(const char* name, int fallback, int* value) {
  if (value == nullptr) {
    return false;
  }
  const char* text = GetEnvNonEmpty(name);
  if (text == nullptr) {
    *value = fallback;
    return fallback > 0;
  }
  char* end = nullptr;
  errno = 0;
  const long parsed = std::strtol(text, &end, 10);
  if (errno != 0 || end == text || *end != '\0' || parsed <= 0 ||
      parsed > 16384) {
    return false;
  }
  *value = static_cast<int>(parsed);
  return true;
}

bool FileExists(const char* path) {
  return path != nullptr && path[0] != '\0' && access(path, R_OK) == 0;
}

const char* WindowsAngleMode() {
  const char* mode = GetEnvNonEmpty("BLOX7_ANGLE");
  return mode != nullptr ? mode : "auto";
}

bool WindowsAngleOff() { return StringEquals(WindowsAngleMode(), "off"); }

bool WindowsAngleWarp() { return StringEquals(WindowsAngleMode(), "warp"); }

bool WindowsAngleForced() {
  return g_auto_angle_retry_attempted ||
         StringEquals(WindowsAngleMode(), "on") || WindowsAngleWarp();
}

std::string WindowsBinFile(const char* name) {
  const char* dir = __w32_exe_dir();
  return std::string(dir != nullptr ? dir : "") + name;
}

bool WindowsAngleBundled() {
  static const bool bundled = [] {
    char exe[4097] = {};
    const ssize_t length = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (length <= 0 || static_cast<std::size_t>(length) >= sizeof exe - 1) {
      return false;
    }
    const std::filesystem::path bin =
        std::filesystem::path(std::string(exe, static_cast<std::size_t>(length)))
            .parent_path();
    for (const char* name : {"libEGL.dll", "libGLESv2.dll", "d3dcompiler_47.dll"}) {
      if (!FileExists((bin / name).string().c_str())) {
        return false;
      }
    }
    return true;
  }();
  return bundled;
}

bool WindowsAngleReady() {
  static const bool ready = [] {
    if (!WindowsAngleBundled()) {
      return false;
    }
    for (const char* name : {"d3dcompiler_47.dll", "libGLESv2.dll", "libEGL.dll"}) {
      const std::string path = WindowsBinFile(name);
      if (__w32_pe_open(path.c_str()) == nullptr) {
        fprintf(stderr, "  [window] cannot load %s\n", path.c_str());
        return false;
      }
    }
    return true;
  }();
  return ready;
}

struct D3DFeatureLevel {
  const char* name;
  int major;
  int minor;
};

bool ParseD3DFeatureLevel(const char* text, int* major, int* minor) {
  static constexpr D3DFeatureLevel kLevels[] = {
      {"9_3", 9, 3}, {"10_0", 10, 0}, {"10_1", 10, 1}, {"11_0", 11, 0},
      {"11_1", 11, 1}};
  if (text == nullptr) {
    return false;
  }
  for (const D3DFeatureLevel& level : kLevels) {
    if (StringEquals(text, level.name)) {
      *major = level.major;
      *minor = level.minor;
      return true;
    }
  }
  return false;
}

void WarnAboutUnknownWindowsAngleSettings() {
  static bool checked = false;
  if (checked) {
    return;
  }
  checked = true;
  const char* mode = GetEnvNonEmpty("BLOX7_ANGLE");
  if (mode != nullptr && !StringEquals(mode, "auto") &&
      !StringEquals(mode, "on") && !StringEquals(mode, "off") &&
      !StringEquals(mode, "warp") && !StringEquals(mode, "retry-test")) {
    fprintf(stderr,
            "  [window] BLOX7_ANGLE=%s is not auto, on, warp or off; using "
            "auto\n",
            mode);
  }
  const char* level = GetEnvNonEmpty("BLOX7_ANGLE_FEATURE_LEVEL");
  int major = 0;
  int minor = 0;
  if (level != nullptr && !ParseD3DFeatureLevel(level, &major, &minor)) {
    fprintf(stderr,
            "  [window] BLOX7_ANGLE_FEATURE_LEVEL=%s is not 9_3, 10_0, 10_1, "
            "11_0 or 11_1; ignored\n",
            level);
  }
}

const char* kAngleD3D11Features[] = {"allowES3OnFL10_0", nullptr};

SDL_EGLAttrib* SDLCALL AngleD3D11PlatformAttributes(void*) {
  SDL_EGLAttrib* attrs =
      static_cast<SDL_EGLAttrib*>(SDL_malloc(sizeof(SDL_EGLAttrib) * 13));
  if (attrs == nullptr) {
    return nullptr;
  }
  int i = 0;
  if (!StringEquals(GetEnvNonEmpty("BLOX7_ANGLE_PRESENT"), "copy")) {
    attrs[i++] = EGL_EXPERIMENTAL_PRESENT_PATH_ANGLE_VAL;
    attrs[i++] = EGL_EXPERIMENTAL_PRESENT_PATH_FAST_ANGLE_VAL;
  }
  attrs[i++] = EGL_PLATFORM_ANGLE_TYPE_ANGLE_VAL;
  attrs[i++] = EGL_PLATFORM_ANGLE_TYPE_D3D11_ANGLE_VAL;
  attrs[i++] = EGL_PLATFORM_ANGLE_DEVICE_TYPE_ANGLE_VAL;
  attrs[i++] = WindowsAngleWarp()
                   ? EGL_PLATFORM_ANGLE_DEVICE_TYPE_D3D_WARP_ANGLE_VAL
                   : EGL_PLATFORM_ANGLE_DEVICE_TYPE_HARDWARE_ANGLE_VAL;
  attrs[i++] = EGL_FEATURE_OVERRIDES_ENABLED_ANGLE_VAL;
  attrs[i++] = reinterpret_cast<SDL_EGLAttrib>(kAngleD3D11Features);
  int major = 0;
  int minor = 0;
  if (ParseD3DFeatureLevel(GetEnvNonEmpty("BLOX7_ANGLE_FEATURE_LEVEL"), &major,
                           &minor)) {
    attrs[i++] = EGL_PLATFORM_ANGLE_MAX_VERSION_MAJOR_ANGLE_VAL;
    attrs[i++] = major;
    attrs[i++] = EGL_PLATFORM_ANGLE_MAX_VERSION_MINOR_ANGLE_VAL;
    attrs[i++] = minor;
  }
  attrs[i++] = EGL_NONE_VAL;
  return attrs;
}

const char* WindowsGlString(unsigned int name) {
  using GetString = const unsigned char* (*)(unsigned int);
  const auto get_string =
      reinterpret_cast<GetString>(SDL_GL_GetProcAddress("glGetString"));
  return get_string != nullptr
             ? reinterpret_cast<const char*>(get_string(name))
             : nullptr;
}

bool ReadWindowsGlesVersion(int* major, int* minor) {
  const char* version = WindowsGlString(0x1F02);
  int parsed_major = 0;
  int parsed_minor = 0;
  if (version == nullptr ||
      std::sscanf(version, "OpenGL ES %d.%d", &parsed_major, &parsed_minor) !=
          2) {
    return false;
  }
  *major = parsed_major;
  *minor = parsed_minor;
  return true;
}

bool LogWindowsGlesProvider() {
  const char* renderer = WindowsGlString(0x1F01);
  const char* version = WindowsGlString(0x1F02);
  const bool angle =
      renderer != nullptr && std::strstr(renderer, "ANGLE") != nullptr;
  fprintf(stderr, "  [window] OpenGL ES from %s: %s, %s\n",
          angle ? "ANGLE" : "the graphics driver",
          renderer != nullptr ? renderer : "(no renderer string)",
          version != nullptr ? version : "(no version string)");
  return angle;
}

void EnableAngleShaderCache() {
  if (IsDisabledEnv("BLOX7_ANGLE_SHADER_CACHE")) {
    return;
  }
  const blox7::runtime::ProcessEnvironment environment;
  const std::string dir =
      (blox7::runtime::RuntimePaths::FromEnvironment(environment).cache_root() /
       "angle")
          .string();
  if (blox7_angle_blob_cache(dir.c_str())) {
    fprintf(stderr, "  [window] ANGLE keeps compiled shaders in %s\n",
            dir.c_str());
  }
}

bool ShouldShowWindowImmediately() {
  if (IsEnabledEnv("BLOX7_HIDE_WINDOW_UNTIL_FIRST_SWAP")) {
    return false;
  }
  return IsEnabledEnv("BLOX7_DEBUG_SHOW_WINDOW_BEFORE_FRAME");
}

bool ConfigureGraphicsBackendBeforeSDL() {
  WarnAboutUnknownWindowsAngleSettings();
  if (WindowsAngleForced()) {
    if (!WindowsAngleReady()) {
      if (g_auto_angle_retry_attempted) {
        SDL_SetError("ANGLE could not be loaded from the runtime's bin folder");
      } else {
        SDL_SetError(
            "BLOX7_ANGLE=%s needs libEGL.dll, libGLESv2.dll and "
            "d3dcompiler_47.dll in the runtime's bin folder",
            WindowsAngleMode());
      }
      fprintf(stderr, "  [window] %s\n", SDL_GetError());
      return false;
    }
    SDL_SetHintWithPriority(SDL_HINT_VIDEO_FORCE_EGL, "1", SDL_HINT_OVERRIDE);
    SDL_SetHintWithPriority(SDL_HINT_OPENGL_ES_DRIVER, "1", SDL_HINT_OVERRIDE);
    SDL_SetHintWithPriority(SDL_HINT_OPENGL_LIBRARY,
                            WindowsBinFile("libGLESv2.dll").c_str(),
                            SDL_HINT_OVERRIDE);
    SDL_SetHintWithPriority(SDL_HINT_EGL_LIBRARY,
                            WindowsBinFile("libEGL.dll").c_str(),
                            SDL_HINT_OVERRIDE);
    SDL_SetHintWithPriority(SDL_HINT_VIDEO_WIN_D3DCOMPILER,
                            WindowsBinFile("d3dcompiler_47.dll").c_str(),
                            SDL_HINT_OVERRIDE);
  }
  if (!WindowsAngleOff() && WindowsAngleBundled()) {
    SDL_SetHintWithPriority(SDL_HINT_VIDEO_EGL_ALLOW_GETDISPLAY_FALLBACK, "0",
                            SDL_HINT_OVERRIDE);
  }

  if (WindowTraceEnabled()) {
    fprintf(stderr, "  [window] graphics backend=%s\n",
            GetEnvNonEmpty("BLOX7_GRAPHICS_BACKEND")
                ? GetEnvNonEmpty("BLOX7_GRAPHICS_BACKEND")
                : "system");
  }
  return true;
}

bool ConfigureGraphicsBackendAfterSDLInit() {
  if (WindowsAngleForced()) {
    if (!SDL_GL_SetAttribute(SDL_GL_EGL_PLATFORM, EGL_PLATFORM_ANGLE_ANGLE_VAL)) {
      fprintf(stderr, "  [window] cannot select the ANGLE EGL platform: %s\n",
              SDL_GetError());
      return false;
    }
    SDL_EGL_SetAttributeCallbacks(AngleD3D11PlatformAttributes, nullptr,
                                  nullptr, nullptr);
    const char* level = GetEnvNonEmpty("BLOX7_ANGLE_FEATURE_LEVEL");
    int major = 0;
    int minor = 0;
    const bool capped = ParseD3DFeatureLevel(level, &major, &minor);
    fprintf(stderr, "  [window] OpenGL ES through ANGLE (Direct3D 11%s%s%s)\n",
            WindowsAngleWarp() ? ", WARP" : "",
            capped ? ", feature level " : "", capped ? level : "");
  } else if (!WindowsAngleOff() && WindowsAngleBundled()) {
    fprintf(stderr,
            "  [window] ANGLE (Direct3D 11) takes over if the graphics driver "
            "has no OpenGL ES 3.0\n");
  }
  return true;
}

void ShowWindowAccordingToStartupMode() {
  if (ShouldShowWindowImmediately()) {
    SDL_ShowWindow(g_state.sdl_window);
    SDL_RaiseWindow(g_state.sdl_window);
    g_state.visible = true;
  } else {
    fprintf(stderr, "  [window] hidden until first Roblox frame\n");
  }
}

void* QueryNativeWindowHandle() {
  SDL_PropertiesID props = SDL_GetWindowProperties(g_state.sdl_window);
  void* hwnd = SDL_GetPointerProperty(
      props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
  if (hwnd != nullptr) {
    return hwnd;
  }

  return g_state.direct_vulkan ? g_state.sdl_window : nullptr;
}

float QueryWindowDpiScale() {
  if (g_state.sdl_window == nullptr) {
    return 1.0f;
  }
  return platform::RobloxUiScale(g_state.sdl_window);
}

void ResolveNativeWindowHandle() {
  g_state.native_window = QueryNativeWindowHandle();
  if (g_state.native_window == nullptr) {
    fprintf(stderr, "  [window] WARNING: no native window found\n");
    return;
  }
  fprintf(stderr, "  [window] native window=%p\n", g_state.native_window);
}

bool ActivateWindowEventLifecycles() {
  g_state.display_refresh =
      platform::QuerySdlDisplayRefreshCapabilities(g_state.sdl_window);
  if (g_state.display_refresh.valid()) {
    fprintf(stderr, "  [window] display refresh current=%.3f Hz modes=%zu\n",
            g_state.display_refresh.current_hz,
            g_state.display_refresh.supported_hz.size());
  } else {
    fprintf(stderr, "  [window] display refresh unavailable\n");
  }
  int pixel_width = 0;
  int pixel_height = 0;
  SDL_GetWindowSizeInPixels(g_state.sdl_window, &pixel_width, &pixel_height);
  if (pixel_width <= 0 || pixel_height <= 0 ||
      g_state.native_window == nullptr) {
    fprintf(stderr,
            "  [window] cannot activate typed surface lifecycle "
            "(native=%p pixels=%dx%d)\n",
            g_state.native_window, pixel_width, pixel_height);
    return false;
  }
  const Status surface_status = g_window_surface_lifecycle.Activate(
      reinterpret_cast<uintptr_t>(g_state.native_window),
      static_cast<uint32_t>(pixel_width), static_cast<uint32_t>(pixel_height),
      QueryWindowDpiScale());
  if (!surface_status.ok()) {
    fprintf(stderr, "  [window] typed surface lifecycle failed: %s\n",
            surface_status.message().c_str());
    return false;
  }
  if (!g_platform_event_observer.Activate()) {
    fprintf(stderr, "  [window] platform event observer is already active\n");
    g_window_surface_lifecycle.Deactivate();
    return false;
  }
  WindowResizeReadinessConfig resize_config;
  resize_config.enabled =
      StringEquals(GetEnvNonEmpty("BLOX7_RESIZE_READINESS"), "1");
  if (resize_config.enabled) {
    g_resize_readiness_gate = std::make_unique<WindowResizeReadinessGate>();
    int logical_width = 0;
    int logical_height = 0;
    if (!SDL_GetWindowSize(g_state.sdl_window, &logical_width,
                           &logical_height) ||
        logical_width <= 0 || logical_height <= 0) {
      fprintf(stderr,
              "  [resize] cannot determine initial SDL logical extent\n");
      g_platform_event_observer.Deactivate();
      g_window_surface_lifecycle.Deactivate();
      g_resize_readiness_gate.reset();
      return false;
    }
    const int fallback_width =
        logical_width >= 960 ? logical_width - 160 : logical_width + 160;
    const int fallback_height =
        logical_height >= 630 ? logical_height - 90 : logical_height + 90;
    int target_logical_width = 0;
    int target_logical_height = 0;
    if (!ParsePositiveInt("BLOX7_RESIZE_WIDTH", fallback_width,
                          &target_logical_width) ||
        !ParsePositiveInt("BLOX7_RESIZE_HEIGHT", fallback_height,
                          &target_logical_height)) {
      fprintf(
          stderr,
          "  [resize] invalid logical target; dimensions must be 1..16384\n");
      g_platform_event_observer.Deactivate();
      g_window_surface_lifecycle.Deactivate();
      g_resize_readiness_gate.reset();
      return false;
    }
    const uint64_t target_pixel_width =
        (static_cast<uint64_t>(target_logical_width) * pixel_width +
         static_cast<uint64_t>(logical_width) / 2) /
        static_cast<uint64_t>(logical_width);
    const uint64_t target_pixel_height =
        (static_cast<uint64_t>(target_logical_height) * pixel_height +
         static_cast<uint64_t>(logical_height) / 2) /
        static_cast<uint64_t>(logical_height);
    if (target_pixel_width > UINT32_MAX || target_pixel_height > UINT32_MAX) {
      fprintf(stderr, "  [resize] scaled target exceeds surface limits\n");
      g_platform_event_observer.Deactivate();
      g_window_surface_lifecycle.Deactivate();
      g_resize_readiness_gate.reset();
      return false;
    }
    resize_config.target_width = static_cast<uint32_t>(target_pixel_width);
    resize_config.target_height = static_cast<uint32_t>(target_pixel_height);
    const Status resize_status = g_resize_readiness_gate->Activate(
        resize_config, g_window_surface_lifecycle.Snapshot());
    if (!resize_status.ok()) {
      fprintf(stderr, "  [resize] readiness activation failed: %s\n",
              resize_status.message().c_str());
      g_platform_event_observer.Deactivate();
      g_window_surface_lifecycle.Deactivate();
      g_resize_readiness_gate.reset();
      return false;
    }
    fprintf(
        stderr,
        "  [resize] armed real compositor resize logical=%dx%d pixels=%ux%u\n",
        target_logical_width, target_logical_height, resize_config.target_width,
        resize_config.target_height);
    g_state.resize_target_logical_width = target_logical_width;
    g_state.resize_target_logical_height = target_logical_height;
  }
  g_state.width = pixel_width;
  g_state.height = pixel_height;
  g_pre_text_input_pump_gate.Activate();
  g_text_input_backend =
      std::make_unique<SdlTextInputBackend>(g_state.sdl_window);
  g_text_input_owner =
      std::make_unique<WindowTextInputOwner>(g_text_input_backend.get());
  g_pointer_capture_backend =
      std::make_unique<SdlPointerCaptureBackend>(g_state.sdl_window);
  g_pointer_capture_owner = std::make_unique<WindowPointerCaptureOwner>(
      g_pointer_capture_backend.get());
  fprintf(stderr,
          "  [input] SDL event pump target=%llu Hz raw-motion, no smoothing\n",
          static_cast<unsigned long long>(kProductionInputPumpHz));
  return true;
}

bool RetryWithAutoAngleFallback(int width, int height, const char* title,
                                const char* failed_step, bool* retried_ok) {
  if (g_auto_angle_retry_attempted || WindowsAngleOff() ||
      WindowsAngleForced() || !WindowsAngleBundled()) {
    fprintf(stderr, "  [window] no ANGLE retry (%s)\n",
            g_auto_angle_retry_attempted ? "already retried"
            : WindowsAngleOff()          ? "BLOX7_ANGLE=off"
            : WindowsAngleForced()       ? "ANGLE was already in use"
                                         : "ANGLE is not bundled");
    return false;
  }
  SDL_QuitSubSystem(SDL_INIT_VIDEO);
  if (SDL_WasInit(SDL_INIT_VIDEO) != 0) {
    fprintf(stderr, "  [window] no ANGLE retry (SDL video is still in use)\n");
    return false;
  }
  g_auto_angle_retry_attempted = true;
  fprintf(stderr,
          "  [window] %s failed; retrying OpenGL ES through ANGLE "
          "(Direct3D 11)\n",
          failed_step ? failed_step : "OpenGL ES setup");
  *retried_ok = Init(width, height, title);
  return true;
}

Status ConfigureWindowStatePersistence(const std::filesystem::path& path) {
  if (g_state.initialised) {
    return Status::Error(
        StatusCode::kFailedPrecondition,
        "window state persistence must be configured before Init");
  }
  if (!path.is_absolute()) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "window state path must be absolute");
  }
  g_window_state_path = path.lexically_normal();
  return Status::Ok();
}

static void NoteAngleCannotDraw() {
  if (!WindowsAngleForced() || g_gpu_lacks_gles3) {
    return;
  }
  g_gpu_lacks_gles3 = true;
  fprintf(stderr, "  %s\n", graphics::kNoGles3Line);
}

bool Init(int width, int height, const char* title) {
  if (g_state.initialised) {
    return true;
  }
  const int requested_width = width;
  const int requested_height = height;

  const Status metadata_status = platform::ConfigureSdlApplicationMetadata();
  if (!metadata_status.ok()) {
    std::fprintf(stderr, "  [window] SDL application metadata failed: %s\n",
                 metadata_status.message().c_str());
    return false;
  }

  SDL_SetHint(SDL_HINT_IME_IMPLEMENTED_UI, "composition");
  // sdl eats the activation click and skips clipping until that button comes back up
  SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");

  g_state.state_persistence_active =
      !g_window_state_path.empty() && !WindowStatePersistenceSuppressed();
  g_state.state_persistence_dirty = false;
  g_state.state_persistence_change_ticks_ns = 0;
  g_state.persisted_window = {};
  g_state.persisted_window.width = width;
  g_state.persisted_window.height = height;
  g_state.persisted_window.configured_width = requested_width;
  g_state.persisted_window.configured_height = requested_height;
  g_state.persisted_window.has_configured_size = true;
  g_state.restored_fullscreen_sync_pending = false;
  if (g_state.state_persistence_active) {
    const WindowStateLoadResult restored = LoadWindowState(g_window_state_path);
    if (!restored) {
      std::fprintf(stderr, "  [window-state] restore ignored: %s\n",
                   restored.status.message().c_str());
    } else if (restored.found) {
      g_state.persisted_window = ReconcileConfiguredSize(
          restored.state, requested_width, requested_height);
      width = g_state.persisted_window.width;
      height = g_state.persisted_window.height;
      if (RestoredSizeApplies(restored.state, requested_width,
                              requested_height)) {
        std::fprintf(stderr,
                     "  [window-state] restored logical=%dx%d fullscreen=%d "
                     "maximized=%d position=%s\n",
                     width, height, restored.state.fullscreen ? 1 : 0,
                     restored.state.maximized ? 1 : 0,
                     restored.state.has_position ? "saved" : "compositor");
      } else {
        std::fprintf(stderr,
                     "  [window-state] configured logical=%dx%d replaces "
                     "saved=%dx%d maximized=%d, starts fullscreen=%d "
                     "maximized=0 position=%s\n",
                     width, height, restored.state.width,
                     restored.state.height,
                     restored.state.maximized ? 1 : 0,
                     restored.state.fullscreen ? 1 : 0,
                     restored.state.has_position ? "saved" : "compositor");
      }
      g_state.restored_fullscreen_sync_pending = true;
    }
  }
  g_state.width = width;
  g_state.height = height;
  g_state.input_test_sequence_queued = false;
  g_state.fullscreen_readiness_requested = false;
  g_state.fullscreen_readiness_return_requested = false;
  g_state.fullscreen_readiness_present_baseline = 0;
  g_fullscreen_request_gate.Reset();
  g_fullscreen_menu_request_gate.Reset();
  g_real_swap_count.store(0, std::memory_order_relaxed);

  if (!ConfigureGraphicsBackendBeforeSDL()) {
    return false;
  }

  if (StringEquals(GetEnvNonEmpty("BLOX7_GRAPHICS_BACKEND"),
                   "direct-vulkan")) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
      fprintf(stderr, "  [window] SDL_Init failed: %s\n", SDL_GetError());
      return false;
    }
    const WindowStartupPresentationPlan presentation =
        RestoredWindowPresentationPlan();
    SDL_WindowFlags window_flags = ApplyHighPixelDensityWindowFlag(
        SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE,
        IsEnabledEnv("BLOX7_WIN_HIGH_DPI"));
    if (presentation.fullscreen_at_creation) {
      window_flags |= SDL_WINDOW_FULLSCREEN;
    }
    if (!ShouldShowWindowImmediately()) {
      window_flags |= SDL_WINDOW_HIDDEN;
    }
    SDL_SetHint(SDL_HINT_WINDOWS_ERASE_BACKGROUND_MODE, "0");
    g_state.sdl_window = SDL_CreateWindow(title ? title : "Roblox",
                                          width, height, window_flags);
    if (g_state.sdl_window == nullptr) {
      fprintf(stderr, "  [window] SDL Vulkan window failed: %s\n",
              SDL_GetError());
      SDL_Quit();
      return false;
    }
    ApplyWindowIcon(g_state.sdl_window);
    ApplyMinimumWindowSize(g_state.sdl_window);
    ApplyRestoredWindowPosition();
    ApplyRestoredWindowMaximization(g_state.sdl_window, presentation);
    ApplyFullscreenRefreshMode(g_state.sdl_window);
    g_state.direct_vulkan = true;
    g_state.initialised = true;
    ResolveNativeWindowHandle();
    if (g_state.native_window == nullptr) {
      g_state.native_window = g_state.sdl_window;
    }
    RedrawWholeClientOnResize(g_state.sdl_window);
    ShowWindowAccordingToStartupMode();
    fprintf(stderr, "  [window] SDL3 direct Vulkan window created (%dx%d)\n",
            width, height);
    if (!g_present_lifecycle.Activate()) {
      fprintf(stderr, "  [window] host present lifecycle is already active\n");
      g_present_lifecycle.Deactivate();
      SDL_DestroyWindow(g_state.sdl_window);
      g_state.sdl_window = nullptr;
      SDL_Quit();
      g_state.native_window = nullptr;
      g_state.direct_vulkan = false;
      g_state.initialised = false;
      return false;
    }
    if (!g_vulkan_surface_recovery_gate.Activate()) {
      fprintf(stderr,
              "  [window] direct Vulkan surface recovery is already active\n");
      g_vulkan_surface_recovery_gate.Deactivate();
      g_present_lifecycle.Deactivate();
      SDL_DestroyWindow(g_state.sdl_window);
      g_state.sdl_window = nullptr;
      SDL_Quit();
      g_state.native_window = nullptr;
      g_state.direct_vulkan = false;
      g_state.initialised = false;
      return false;
    }
    if (!g_vulkan_present_progress_gate.Activate()) {
      fprintf(stderr,
              "  [window] Vulkan present progress gate is already active\n");
      g_vulkan_present_progress_gate.Deactivate();
      g_vulkan_surface_recovery_gate.Deactivate();
      g_present_lifecycle.Deactivate();
      SDL_DestroyWindow(g_state.sdl_window);
      g_state.sdl_window = nullptr;
      SDL_Quit();
      g_state.native_window = nullptr;
      g_state.direct_vulkan = false;
      g_state.initialised = false;
      return false;
    }
    if (!ActivateWindowEventLifecycles()) {
      g_vulkan_present_progress_gate.Deactivate();
      g_vulkan_surface_recovery_gate.Deactivate();
      g_present_lifecycle.Deactivate();
      SDL_DestroyWindow(g_state.sdl_window);
      g_state.sdl_window = nullptr;
      SDL_Quit();
      g_state.native_window = nullptr;
      g_state.direct_vulkan = false;
      g_state.initialised = false;
      return false;
    }
    return true;
  }

  if (WindowTraceEnabled()) {
    fprintf(stderr, "  [window] Init requested width=%d height=%d title=%s\n",
            width, height, title ? title : "(null)");
  }

  if (!SDL_Init(SDL_INIT_VIDEO)) {
    fprintf(stderr, "  [window] SDL_Init failed: %s\n", SDL_GetError());
    return false;
  }
  if (WindowTraceEnabled()) {
    fprintf(stderr, "  [window] SDL_Init(SDL_INIT_VIDEO) succeeded (video=%s)\n",
            SDL_GetCurrentVideoDriver());
  }

  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
  SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
  SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

  if (!ConfigureGraphicsBackendAfterSDLInit()) {
    SDL_Quit();
    return false;
  }

  const WindowStartupPresentationPlan presentation =
      RestoredWindowPresentationPlan();
  SDL_WindowFlags window_flags = ApplyHighPixelDensityWindowFlag(
      SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE,
      IsEnabledEnv("BLOX7_WIN_HIGH_DPI"));
  if (presentation.fullscreen_at_creation) {
    window_flags |= SDL_WINDOW_FULLSCREEN;
  }
  if (!ShouldShowWindowImmediately()) {
    window_flags |= SDL_WINDOW_HIDDEN;
  }

  g_state.sdl_window = SDL_CreateWindow(title ? title : "Roblox",
                                        width, height, window_flags);
  if (!g_state.sdl_window) {
    fprintf(stderr, "  [window] SDL_CreateWindow failed: %s\n", SDL_GetError());
    bool retried_ok = false;
    if (RetryWithAutoAngleFallback(requested_width, requested_height, title,
                                   "SDL_CreateWindow", &retried_ok)) {
      return retried_ok;
    }
    NoteAngleCannotDraw();
    SDL_Quit();
    return false;
  }
  ApplyWindowIcon(g_state.sdl_window);
  ApplyMinimumWindowSize(g_state.sdl_window);
  ApplyRestoredWindowPosition();
  ApplyRestoredWindowMaximization(g_state.sdl_window, presentation);
  ApplyFullscreenRefreshMode(g_state.sdl_window);
  fprintf(stderr, "  [window] SDL3 window created (%dx%d)\n", width, height);
  ShowWindowAccordingToStartupMode();

  SDL_GLContext sdl_context = SDL_GL_CreateContext(g_state.sdl_window);
  if (!sdl_context && IsEnabledEnv("BLOX7_ALLOW_GLES2_RESEARCH")) {
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    sdl_context = SDL_GL_CreateContext(g_state.sdl_window);
  }
  if (!sdl_context) {
    fprintf(stderr,
            "  [window] OpenGL ES 3.0 context required; "
            "SDL_GL_CreateContext failed: %s\n",
            SDL_GetError());
    SDL_DestroyWindow(g_state.sdl_window);
    g_state.sdl_window = nullptr;
    bool retried_ok = false;
    if (RetryWithAutoAngleFallback(requested_width, requested_height, title,
                                   "SDL_GL_CreateContext", &retried_ok)) {
      return retried_ok;
    }
    NoteAngleCannotDraw();
    SDL_Quit();
    return false;
  }

  if (!SDL_GL_MakeCurrent(g_state.sdl_window, sdl_context)) {
    fprintf(stderr, "  [window] SDL_GL_MakeCurrent failed: %s\n",
            SDL_GetError());
    SDL_GL_DestroyContext(sdl_context);
    SDL_DestroyWindow(g_state.sdl_window);
    g_state.sdl_window = nullptr;
    bool retried_ok = false;
    if (RetryWithAutoAngleFallback(requested_width, requested_height, title,
                                   "SDL_GL_MakeCurrent", &retried_ok)) {
      return retried_ok;
    }
    NoteAngleCannotDraw();
    SDL_Quit();
    return false;
  }
  if (WindowTraceEnabled()) {
    fprintf(stderr, "  [window] SDL_GL_MakeCurrent succeeded (context=%p)\n",
            static_cast<void*>(sdl_context));
  }
  int context_major = 0;
  int context_minor = 0;
  if (LogWindowsGlesProvider()) {
    EnableAngleShaderCache();
  }
  const bool context_version_available =
      ReadWindowsGlesVersion(&context_major, &context_minor);
  if (context_version_available) {
    fprintf(stderr, "  [window] OpenGL ES context version=%d.%d\n",
            context_major, context_minor);
  } else {
    fprintf(stderr, "  [window] the context is not OpenGL ES\n");
  }
  const bool roblox_draws_here =
      graphics::RobloxAcceptsGlesVersion(WindowsGlString(0x1F02));
  const bool unsupported_version =
      !roblox_draws_here && !IsEnabledEnv("BLOX7_ALLOW_GLES2_RESEARCH");
  bool refuse_context = unsupported_version;
  if (!WindowsAngleForced() && StringEquals(WindowsAngleMode(), "retry-test")) {
    fprintf(stderr,
            "  [window] BLOX7_ANGLE=retry-test: refusing the graphics "
            "driver's context\n");
    refuse_context = true;
  }
  if (refuse_context) {
    if (unsupported_version) {
      fprintf(stderr,
              "  [window] refusing unsupported OpenGL ES context; version "
              "3.0 or newer is required\n");
    }
    SDL_GL_MakeCurrent(g_state.sdl_window, nullptr);
    SDL_GL_DestroyContext(sdl_context);
    SDL_DestroyWindow(g_state.sdl_window);
    g_state.sdl_window = nullptr;
    bool retried_ok = false;
    if (RetryWithAutoAngleFallback(requested_width, requested_height, title,
                                   "OpenGL ES version check", &retried_ok)) {
      return retried_ok;
    }
    NoteAngleCannotDraw();
    SDL_Quit();
    return false;
  }

  static int win32_egl_display, win32_egl_config;
  g_state.egl_display = &win32_egl_display;
  g_state.egl_config = &win32_egl_config;
  g_state.egl_surface = static_cast<EGLSurface>(g_state.sdl_window);
  g_state.egl_context = static_cast<EGLContext>(sdl_context);

  if (!g_state.egl_display || !g_state.egl_surface || !g_state.egl_context) {
    fprintf(stderr,
            "  [window] SDL_EGL handles query failed (display=%p, surface=%p, "
            "context=%p)\n",
            g_state.egl_display, g_state.egl_surface, g_state.egl_context);
    SDL_GL_DestroyContext(sdl_context);
    SDL_DestroyWindow(g_state.sdl_window);
    g_state.sdl_window = nullptr;
    g_state.egl_display = nullptr;
    g_state.egl_config = nullptr;
    g_state.egl_surface = nullptr;
    g_state.egl_context = nullptr;
    bool retried_ok = false;
    if (RetryWithAutoAngleFallback(requested_width, requested_height, title,
                                   "SDL_EGL handle query", &retried_ok)) {
      return retried_ok;
    }
    SDL_Quit();
    return false;
  }
  fprintf(stderr, "  [window] EGL context and surface initialized via SDL3\n");
  if (WindowTraceEnabled()) {
    fprintf(
        stderr,
        "  [window] EGL handles display=%p config=%p surface=%p context=%p\n",
        g_state.egl_display, g_state.egl_config, g_state.egl_surface,
        g_state.egl_context);
  }

  g_state.egl_swap_interval = [](EGLDisplay, EGLint interval) -> EGLBoolean {
    return SetWindowsSwapInterval(interval) ? EGL_TRUE_B : EGL_FALSE_B;
  };
  const PFN_eglGetConfigAttrib get_config_attrib = nullptr;
  EGLint min_interval = 0;
  EGLint max_interval = 1;
  g_state.min_swap_interval = min_interval;
  g_state.max_swap_interval = max_interval;
  if (get_config_attrib != nullptr &&
      get_config_attrib(g_state.egl_display, g_state.egl_config,
                        EGL_MIN_SWAP_INTERVAL_VAL, &min_interval) &&
      get_config_attrib(g_state.egl_display, g_state.egl_config,
                        EGL_MAX_SWAP_INTERVAL_VAL, &max_interval) &&
      min_interval >= 0 && max_interval >= min_interval) {
    g_state.min_swap_interval = min_interval;
    g_state.max_swap_interval = max_interval;
  }
  if (graphics::CachedPresentModePolicy() !=
          graphics::PresentModePolicy::kHostDefault && !SetSwapInterval(1)) {
    SDL_GL_DestroyContext(sdl_context);
    SDL_DestroyWindow(g_state.sdl_window);
    g_state.sdl_window = nullptr;
    g_state.egl_display = nullptr;
    g_state.egl_config = nullptr;
    g_state.egl_surface = nullptr;
    g_state.egl_context = nullptr;
    g_state.egl_swap_interval = nullptr;
    bool retried_ok = false;
    if (RetryWithAutoAngleFallback(requested_width, requested_height, title,
                                   "SDL_GL_SetSwapInterval", &retried_ok)) {
      return retried_ok;
    }
    SDL_Quit();
    return false;
  }

  ResolveNativeWindowHandle();

  SDL_GL_MakeCurrent(g_state.sdl_window, nullptr);
  if (WindowTraceEnabled()) {
    fprintf(stderr, "  [window] SDL_GL_MakeCurrent(nullptr) on main thread\n");
  }

  g_state.initialised = true;
  if (!g_present_lifecycle.Activate()) {
    fprintf(stderr, "  [window] host present lifecycle is already active\n");
    Shutdown();
    return false;
  }
  if (!ActivateWindowEventLifecycles()) {
    Shutdown();
    return false;
  }
  fprintf(stderr,
          "  [window] window initialisation complete (context NOT yet "
          "current, call MakeCurrentOnThread from engine thread)\n");
  return true;
}

bool MakeCurrentOnThread() {
  if (!g_state.initialised || !g_state.egl_context) {
    if (WindowTraceEnabled()) {
      fprintf(stderr,
              "  [window] MakeCurrentOnThread skipped (initialised=%d "
              "context=%p)\n",
              g_state.initialised ? 1 : 0, g_state.egl_context);
    }
    return false;
  }
  if (!SDL_GL_MakeCurrent(g_state.sdl_window,
                          static_cast<SDL_GLContext>(g_state.egl_context))) {
    fprintf(stderr, "  [window] MakeCurrentOnThread failed: %s\n",
            SDL_GetError());
    return false;
  }
  fprintf(stderr, "  [window] EGL context now current on engine thread\n");
  return true;
}

bool ReleaseCurrentOnThread() {
  if (!g_state.initialised || !g_state.sdl_window) {
    return false;
  }
  if (g_state.egl_context == nullptr) {
    return true;
  }
  if (!SDL_GL_MakeCurrent(g_state.sdl_window, nullptr)) {
    fprintf(stderr, "  [window] ReleaseCurrentOnThread failed: %s\n",
            SDL_GetError());
    return false;
  }
  if (WindowTraceEnabled()) {
    fprintf(stderr, "  [window] EGL context released on current thread\n");
  }
  return true;
}

bool IsInitialised() { return g_state.initialised; }
bool HasPresentedFrame() { return g_real_swap_count.load() > 0; }

void RecordFirstPresentTime(int present_count) {
  if (present_count != 1) {
    return;
  }
  g_first_present_ticks_ns.store(SDL_GetTicksNS(), std::memory_order_release);
}
int GetWidth() { return g_state.width; }
int GetHeight() { return g_state.height; }

void* GetEGLDisplay() { return g_state.egl_display; }
void* GetEGLSurface() { return g_state.egl_surface; }
void* GetEGLContext() { return g_state.egl_context; }
void* GetEGLConfig() { return g_state.egl_config; }
void* GetNativeWindow() { return g_state.native_window; }
void* GetBackendWindow() { return g_state.sdl_window; }
bool UsesDirectVulkan() { return g_state.direct_vulkan; }
bool PollWindowSurfaceEvent(WindowSurfaceEvent* event) {
  return g_window_surface_lifecycle.Poll(event);
}
WindowSurfaceSnapshot GetWindowSurfaceSnapshot() {
  return g_window_surface_lifecycle.Snapshot();
}

// vmware's own interval 1 blocked every other swap, so wait on dwm instead
struct DesktopCompositorClock {
  int32_t (__attribute__((ms_abi)) * is_composition_enabled)(int32_t*) = nullptr;
  int32_t (__attribute__((ms_abi)) * flush)() = nullptr;

  DesktopCompositorClock() {
    void* dwmapi = __w32_pe_open("dwmapi.dll");
    if (dwmapi != nullptr) {
      is_composition_enabled = reinterpret_cast<decltype(is_composition_enabled)>(
          __w32_pe_symbol(dwmapi, "DwmIsCompositionEnabled"));
      flush = reinterpret_cast<decltype(flush)>(__w32_pe_symbol(dwmapi, "DwmFlush"));
    }
  }
  bool Composited() const {
    int32_t enabled = 0;
    return is_composition_enabled != nullptr && flush != nullptr &&
           is_composition_enabled(&enabled) >= 0 && enabled != 0;
  }
};

const DesktopCompositorClock& CompositorClock() {
  static const DesktopCompositorClock clock;
  return clock;
}

std::atomic<bool> g_compositor_paced{false};

bool SetWindowsSwapInterval(int interval) {
  const bool paced = interval >= 1 && CompositorClock().Composited();
  if (!SDL_GL_SetSwapInterval(paced ? 0 : interval)) {
    return false;
  }
  if (paced && !g_compositor_paced.exchange(true, std::memory_order_relaxed)) {
    std::fprintf(stderr, "  [window] swap interval 1 paced by the desktop compositor\n");
  } else if (!paced) {
    g_compositor_paced.store(false, std::memory_order_relaxed);
  }
  return true;
}

void WaitForCompositor() {
  if (!g_compositor_paced.load(std::memory_order_relaxed)) {
    return;
  }
  if (CompositorClock().flush() < 0) {
    g_compositor_paced.store(false, std::memory_order_relaxed);
    (void)SDL_GL_SetSwapInterval(1);
    std::fprintf(stderr, "  [window] desktop composition is off: the driver paces swaps\n");
  }
}

void UpdateWindowTextLayer() {
  void* hwnd =
      g_state.sdl_window != nullptr
          ? SDL_GetPointerProperty(SDL_GetWindowProperties(g_state.sdl_window),
                                   SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr)
          : nullptr;
  if (!::blox7::win32::UpdateTextLayer(hwnd)) {
    return;
  }
}

std::atomic<void*> g_present_window{nullptr};
std::atomic<uint64_t> g_present_window_ticks_ns{0};
std::atomic<bool> g_present_window_closing{false};

void PublishPresentWindow() {
  void* hwnd =
      g_state.sdl_window != nullptr
          ? SDL_GetPointerProperty(SDL_GetWindowProperties(g_state.sdl_window),
                                   SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr)
          : nullptr;
  g_present_window.store(hwnd, std::memory_order_release);
  g_present_window_closing.store(g_state.quit_requested,
                                 std::memory_order_release);
  g_present_window_ticks_ns.store(SDL_GetTicksNS(), std::memory_order_release);
}

bool PresentWindowHidden() {
  static void* const user32 = __w32_pe_open("user32.dll");
  static const auto is_iconic =
      reinterpret_cast<int32_t(__attribute__((ms_abi)) *)(void*)>(
          user32 != nullptr ? __w32_pe_symbol(user32, "IsIconic") : nullptr);
  static const auto client_rect =
      reinterpret_cast<int32_t(__attribute__((ms_abi)) *)(void*, int32_t*)>(
          user32 != nullptr ? __w32_pe_symbol(user32, "GetClientRect")
                            : nullptr);
  void* hwnd = g_present_window.load(std::memory_order_acquire);
  const uint64_t published =
      g_present_window_ticks_ns.load(std::memory_order_acquire);
  if (hwnd == nullptr || is_iconic == nullptr || client_rect == nullptr ||
      (!g_present_window_closing.load(std::memory_order_acquire) &&
       SDL_GetTicksNS() - published >= 1000000000ULL)) {
    return false;
  }
  if (is_iconic(hwnd) != 0) {
    return true;
  }
  int32_t client[4] = {};
  return client_rect(hwnd, client) != 0 &&
         (client[2] <= client[0] || client[3] <= client[1]);
}

bool OnPumpThread() {
  return SDL_GetCurrentThreadID() ==
         g_pump_thread_id.load(std::memory_order_acquire);
}

extern "C" bool blox7_window_holds_presents() {
  return !OnPumpThread() &&
         !g_present_window_closing.load(std::memory_order_acquire) &&
         (g_fullscreen_switching.load(std::memory_order_acquire) ||
          PresentWindowHidden());
}

extern "C" bool blox7_window_hides_presents() {
  return !OnPumpThread() &&
         !g_fullscreen_switching.load(std::memory_order_acquire) &&
         PresentWindowHidden();
}

Status RecordResizeReadinessSurfaceCommit(const WindowSurfaceEvent& event) {
  if (g_resize_readiness_gate == nullptr) {
    return Status::Ok();
  }
  const WindowResizeReadinessState before =
      g_resize_readiness_gate->Snapshot().state;
  const Status status =
      g_resize_readiness_gate->RecordCommittedSurfaceEvent(event);
  const WindowResizeReadinessSnapshot after =
      g_resize_readiness_gate->Snapshot();
  if (status.ok() && before == WindowResizeReadinessState::kResizeRequested &&
      after.state == WindowResizeReadinessState::kSurfaceCommitted) {
    fprintf(
        stderr,
        "  [resize] typed JNI surface commit generation=%llu pixels=%ux%u\n",
        static_cast<unsigned long long>(event.surface.generation),
        event.surface.width, event.surface.height);
  }
  return status;
}

Status StopResizeReadiness() {
  if (g_resize_readiness_gate == nullptr) {
    return Status::Ok();
  }
  const Status status = g_resize_readiness_gate->RecordStopped();
  if (status.ok() && g_resize_readiness_gate->Snapshot().state ==
                         WindowResizeReadinessState::kStopped) {
    fprintf(stderr, "  [resize] readiness completed: Stopped\n");
  }
  return status;
}

Status ResizeReadinessCompletionStatus() {
  return g_resize_readiness_gate == nullptr
             ? Status::Ok()
             : g_resize_readiness_gate->CompletionStatus();
}
WindowViewportSnapshot GetWindowViewportSnapshot() {
  WindowViewportSnapshot snapshot;
  if (g_state.sdl_window == nullptr) {
    return snapshot;
  }
  SDL_GetWindowSize(g_state.sdl_window, &snapshot.logical_width,
                    &snapshot.logical_height);
  SDL_GetWindowSizeInPixels(g_state.sdl_window, &snapshot.pixel_width,
                            &snapshot.pixel_height);
  snapshot.dpi_scale = QueryWindowDpiScale();
  return snapshot;
}

platform::DisplayRefreshCapabilities GetDisplayRefreshCapabilities() {
  platform::DisplayRefreshCapabilities caps = g_state.display_refresh;
  if (!UnthrottledPresentationRequested() || !caps.valid()) {
    return caps;
  }
  constexpr float kUnthrottledEngineHz = 240.0f;
  if (caps.current_hz >= kUnthrottledEngineHz) {
    return caps;
  }
  static bool logged = false;
  if (!logged) {
    logged = true;
    std::fprintf(stderr,
                 "  [window] advertising %.2f Hz to engine (display %.2f Hz, "
                 "unthrottled)\n",
                 static_cast<double>(kUnthrottledEngineHz),
                 static_cast<double>(caps.current_hz));
  }
  caps.supported_hz.push_back(kUnthrottledEngineHz);
  return platform::NormalizeDisplayRefreshCapabilities(
      kUnthrottledEngineHz, std::move(caps.supported_hz));
}
bool SetPlatformEventObserver(PlatformEventObserver observer, void* context) {
  return g_platform_event_observer.Register(observer, context);
}
void ClearPlatformEventObserver() { g_platform_event_observer.Clear(); }

bool SetMouseLockQueryCallback(MouseLockQueryCallback callback, void* context) {
  return g_pointer_capture_owner != nullptr &&
         g_pointer_capture_owner->RegisterQuery(callback, context);
}

void ClearMouseLockQueryCallback() {
  if (g_pointer_capture_owner != nullptr) {
    g_pointer_capture_owner->ClearQuery();
  }
}
bool SetPreTextInputPumpCallback(PreTextInputPumpCallback callback,
                                 void* context) {
  return g_pre_text_input_pump_gate.Register(callback, context);
}
void ClearPreTextInputPumpCallback() { g_pre_text_input_pump_gate.Clear(); }
void SetWindowTextInputOwnerEnabled(bool enabled) {
  if (g_text_input_owner != nullptr) {
    g_text_input_owner->SetEnabled(enabled);
  }
}
bool RequestShowTextInput(uint64_t generation, const TextInputArea& area,
                          const TextInputOptions& options) {
  return g_text_input_owner != nullptr &&
         g_text_input_owner->RequestShowTextInput(generation, area, options);
}
bool RequestHideTextInput(uint64_t generation) {
  return g_text_input_owner != nullptr &&
         g_text_input_owner->RequestHideTextInput(generation);
}
void* GetGLProcAddress(const char* name) {
  if (!g_state.initialised || name == nullptr || name[0] == '\0') {
    return nullptr;
  }
  return reinterpret_cast<void*>(SDL_GL_GetProcAddress(name));
}

void ShowIfHidden() {
  if (!g_state.initialised || !g_state.sdl_window || g_state.visible ||
      IsDisabledEnv("BLOX7_SHOW_WINDOW_ON_FIRST_SWAP")) {
    return;
  }
  // ShowWindow from another thread can deadlock on this very frame, the pump does it
  if (SDL_GetCurrentThreadID() != g_pump_thread_id.load(std::memory_order_acquire)) {
    g_show_requested.store(true, std::memory_order_release);
    return;
  }
  SDL_ShowWindow(g_state.sdl_window);
  SDL_RaiseWindow(g_state.sdl_window);
  g_state.visible = true;
  fprintf(stderr, "  [window] shown on first Roblox frame\n");
}

extern "C" void blox7_window_show_if_hidden() { ShowIfHidden(); }

extern "C" bool blox7_window_swap_buffers() { return SwapBuffers(); }

extern "C" bool blox7_window_set_swap_interval(int interval) {
  return SetSwapInterval(interval);
}

extern "C" bool blox7_window_make_current() { return MakeCurrentOnThread(); }

extern "C" bool blox7_window_release_current() {
  return ReleaseCurrentOnThread();
}

extern "C" bool blox7_window_has_presented_frame() {
  return HasPresentedFrame();
}

extern "C" int blox7_window_width() {
  const auto surface = g_window_surface_lifecycle.Snapshot();
  return surface.generation != 0 ? static_cast<int>(surface.width) : GetWidth();
}

extern "C" int blox7_window_height() {
  const auto surface = g_window_surface_lifecycle.Snapshot();
  return surface.generation != 0 ? static_cast<int>(surface.height) : GetHeight();
}

extern "C" void* blox7_gl_proc_address(const char* name) {
  return GetGLProcAddress(name);
}

bool SetSwapInterval(int interval) {
  if (g_state.egl_display == nullptr || g_state.egl_context == nullptr ||
      g_state.egl_swap_interval == nullptr) {
    return false;
  }
  const auto policy = graphics::CachedPresentModePolicy();
  const int requested = graphics::ResolveSwapInterval(policy, interval);
  if (policy != graphics::PresentModePolicy::kHostDefault && requested == 1 &&
      (g_state.min_swap_interval > 1 || g_state.max_swap_interval < 1)) {
    std::fprintf(stderr, "  [window] driver does not support swap interval 1\n");
    return false;
  }
  int applied = std::clamp(requested, g_state.min_swap_interval,
                            g_state.max_swap_interval);
  if (!g_state.egl_swap_interval(g_state.egl_display, applied)) {
    if (applied != 0 || g_state.max_swap_interval < 1 ||
        !g_state.egl_swap_interval(g_state.egl_display, 1)) {
      std::fprintf(stderr, "  [window] eglSwapInterval(%d) failed\n", applied);
      return false;
    }
    applied = 1;
  }
  static std::atomic<bool> logged{false};
  if (!logged.exchange(true, std::memory_order_relaxed)) {
    std::fprintf(stderr, "  [window] present_mode=%s swap_interval=%d%s\n",
                 graphics::PresentModePolicyName(policy), applied,
                 policy == graphics::PresentModePolicy::kMailbox
                     ? " (EGL has no mailbox selection; using FIFO)"
                     : applied != requested ? " (driver fallback)" : "");
  }
  return true;
}

bool SwapBuffers() {
  auto present_scope = g_present_lifecycle.EnterPresent();
  if (!present_scope) {
    return false;
  }
  if (!g_state.initialised || !g_state.sdl_window) {
    if (WindowTraceEnabled()) {
      fprintf(
          stderr, "  [window] SwapBuffers skipped (initialised=%d window=%p)\n",
          g_state.initialised ? 1 : 0, static_cast<void*>(g_state.sdl_window));
    }
    return false;
  }
  if (g_state.egl_display == nullptr || g_state.egl_surface == nullptr ||
      g_state.egl_context == nullptr) {
    if (WindowTraceEnabled()) {
      fprintf(stderr,
              "  [window] SwapBuffers rejected: real EGL "
              "display/surface/context is unavailable\n");
    }
    return false;
  }

  static const bool stall_log = std::getenv("BLOX7_STALL_LOG") != nullptr;
  const uint64_t stall_start_ns = stall_log ? SDL_GetTicksNS() : 0;
  if (!SDL_GL_SwapWindow(g_state.sdl_window)) {
    fprintf(stderr, "  [window] SDL_GL_SwapWindow failed: %s\n",
            SDL_GetError());
    return false;
  }
  const uint64_t stall_swap_ns = stall_log ? SDL_GetTicksNS() : 0;
  WaitForCompositor();
  if (stall_log) {
    static uint64_t last_ns = 0;
    const uint64_t now_ns = SDL_GetTicksNS();
    const uint64_t gap_ns = last_ns != 0 ? now_ns - last_ns : 0;
    last_ns = now_ns;
    if (gap_ns > 25000000ULL) {
      std::fprintf(stderr, "  [stall] t=%.3f gap=%.1f ms swap=%.1f ms flush=%.1f ms\n",
                   now_ns / 1e9, gap_ns / 1e6,
                   (stall_swap_ns - stall_start_ns) / 1e6,
                   (now_ns - stall_swap_ns) / 1e6);
    }
  }

  const int swap_count =
      g_real_swap_count.fetch_add(1, std::memory_order_relaxed) + 1;
  WindowResizeReadinessState resize_state_before =
      WindowResizeReadinessState::kDisabled;
  if (g_resize_readiness_gate != nullptr) {
    resize_state_before = g_resize_readiness_gate->Snapshot().state;
    const Status resize_status = g_resize_readiness_gate->RecordPresent(
        static_cast<uint64_t>(swap_count));
    if (!resize_status.ok()) {
      fprintf(stderr, "  [resize] present evidence rejected: %s\n",
              resize_status.message().c_str());
    } else if (resize_state_before ==
                   WindowResizeReadinessState::kSurfaceCommitted &&
               g_resize_readiness_gate->Snapshot().state ==
                   WindowResizeReadinessState::kPostRebindPresented) {
      fprintf(stderr,
              "  [resize] post-rebind host present backend=opengl frame=%d\n",
              swap_count);
    }
  }
  RecordFirstPresentTime(swap_count);
  present_scope.NotifyObserver(static_cast<uint64_t>(swap_count));
  ShowIfHidden();
  if (swap_count == 1) {
    fprintf(stderr, "  [window] first Roblox frame presented\n");
  }
  if (WindowTraceEnabled() && (swap_count <= 20 || swap_count % 60 == 0)) {
    fprintf(stderr,
            "  [window] SwapBuffers #%d window=%p surface=%p context=%p\n",
            swap_count, static_cast<void*>(g_state.sdl_window),
            g_state.egl_surface, g_state.egl_context);
  }
  return true;
}

void NoteVulkanPresent() {
  auto present_scope = g_present_lifecycle.EnterPresent();
  if (!present_scope) {
    return;
  }
  if (!g_state.initialised || !g_state.direct_vulkan ||
      g_state.sdl_window == nullptr) {
    return;
  }
  g_vulkan_surface_recovery_gate.NotifyPresent();
  const int present_count =
      g_real_swap_count.fetch_add(1, std::memory_order_relaxed) + 1;
  const uint64_t now_ticks_ns = SDL_GetTicksNS();
  g_vulkan_present_progress_gate.NotifyFramePresented(
      now_ticks_ns, static_cast<uint64_t>(present_count),
      SDL_GetCurrentThreadID());
  WindowResizeReadinessState resize_state_before =
      WindowResizeReadinessState::kDisabled;
  if (g_resize_readiness_gate != nullptr) {
    resize_state_before = g_resize_readiness_gate->Snapshot().state;
    const Status resize_status = g_resize_readiness_gate->RecordPresent(
        static_cast<uint64_t>(present_count));
    if (!resize_status.ok()) {
      fprintf(stderr, "  [resize] present evidence rejected: %s\n",
              resize_status.message().c_str());
    } else if (resize_state_before ==
                   WindowResizeReadinessState::kSurfaceCommitted &&
               g_resize_readiness_gate->Snapshot().state ==
                   WindowResizeReadinessState::kPostRebindPresented) {
      fprintf(stderr,
              "  [resize] post-rebind host present backend=vulkan frame=%d\n",
              present_count);
    }
  }
  RecordFirstPresentTime(present_count);
  present_scope.NotifyObserver(static_cast<uint64_t>(present_count));
  ShowIfHidden();
  if (present_count == 1) {
    fprintf(stderr, "  [window] first Roblox Vulkan frame presented\n");
  }
  if (WindowTraceEnabled() &&
      (present_count <= 20 || present_count % 60 == 0)) {
    fprintf(stderr, "  [window] vkQueuePresentKHR #%d window=%p\n",
            present_count, static_cast<void*>(g_state.sdl_window));
  }
}

void NoteVulkanSurfaceOutOfDate() {
  g_vulkan_surface_recovery_gate.NotifyOutOfDate();
}

bool SetPresentObserver(PresentObserver observer, void* context) {
  return g_present_lifecycle.Register(observer, context);
}

void ClearPresentObserver() { g_present_lifecycle.Clear(); }

ScopedPresentObserver::~ScopedPresentObserver() { Reset(); }

bool ScopedPresentObserver::Register(PresentObserver observer, void* context) {
  if (registered_ || !SetPresentObserver(observer, context)) {
    return false;
  }
  registered_ = true;
  return true;
}

void ScopedPresentObserver::Reset() {
  if (!registered_) {
    return;
  }
  ClearPresentObserver();
  registered_ = false;
}

extern "C" void* blox7_window_backend_window() { return GetBackendWindow(); }

extern "C" bool blox7_window_uses_direct_vulkan() {
  return UsesDirectVulkan();
}

extern "C" void blox7_window_note_vulkan_present() { NoteVulkanPresent(); }

extern "C" void blox7_window_note_vulkan_host_present_begin() {
  g_vulkan_host_present_sequence =
      g_vulkan_present_progress_gate.NotifyHostPresentBegin(
          SDL_GetTicksNS(), SDL_GetCurrentThreadID());
}

extern "C" void blox7_window_note_vulkan_host_present_end(
    std::int32_t) {
  g_vulkan_present_progress_gate.NotifyHostPresentEnd(
      g_vulkan_host_present_sequence);
  g_vulkan_host_present_sequence = 0;
}

extern "C" std::uint64_t blox7_window_note_vulkan_call_begin(
    const char* call_name) {
  return g_vulkan_present_progress_gate.NotifyCallBegin(
      call_name, SDL_GetTicksNS(), SDL_GetCurrentThreadID());
}

extern "C" void blox7_window_note_vulkan_call_end(std::uint64_t sequence,
                                                     std::int32_t result) {
  g_vulkan_present_progress_gate.NotifyCallEnd(
      sequence, result, SDL_GetTicksNS(), SDL_GetCurrentThreadID());
}

extern "C" void blox7_window_note_vulkan_surface_out_of_date() {
  NoteVulkanSurfaceOutOfDate();
}

bool ShouldIgnoreCloseRequest() {
  const char* explicit_ignore = std::getenv("BLOX7_IGNORE_WINDOW_CLOSE");
  if (explicit_ignore && explicit_ignore[0] != '\0' &&
      std::strcmp(explicit_ignore, "0") != 0) {
    return true;
  }
  const char* keepalive_ms = std::getenv("BLOX7_KEEPALIVE_MS");
  return keepalive_ms && keepalive_ms[0] != '\0' &&
         std::strcmp(keepalive_ms, "0") != 0;
}

void MaybeQueueInputReadinessSequence() {
  static const bool wanted =
      StringEquals(GetEnvNonEmpty("BLOX7_INPUT_TEST_CLICK"), "1");
  if (g_state.input_test_sequence_queued || !wanted ||
      !HasPresentedFrame() || !g_platform_event_observer.HasObserver() ||
      g_state.sdl_window == nullptr) {
    return;
  }

  int logical_width = 0;
  int logical_height = 0;
  if (!SDL_GetWindowSize(g_state.sdl_window, &logical_width, &logical_height) ||
      logical_width <= 0 || logical_height <= 0) {
    return;
  }
  const float x = static_cast<float>(logical_width) * 0.5f;
  const float y = static_cast<float>(logical_height) * 0.5f;
  const SDL_WindowID window_id = SDL_GetWindowID(g_state.sdl_window);

  SDL_Event focus{};
  focus.type = SDL_EVENT_WINDOW_FOCUS_GAINED;
  focus.window.windowID = window_id;
  const bool needs_focus =
      (SDL_GetWindowFlags(g_state.sdl_window) & SDL_WINDOW_INPUT_FOCUS) == 0;

  SDL_Event motion{};
  motion.type = SDL_EVENT_MOUSE_MOTION;
  motion.motion.windowID = window_id;
  motion.motion.x = x;
  motion.motion.y = y;

  SDL_Event down{};
  down.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
  down.button.windowID = window_id;
  down.button.button = SDL_BUTTON_LEFT;
  down.button.down = true;
  down.button.clicks = 1;
  down.button.x = x;
  down.button.y = y;

  SDL_Event up = down;
  up.type = SDL_EVENT_MOUSE_BUTTON_UP;
  up.button.down = false;

  SDL_Event touch_down{};
  touch_down.type = SDL_EVENT_FINGER_DOWN;
  touch_down.tfinger.windowID = window_id;
  touch_down.tfinger.touchID = 1;
  touch_down.tfinger.fingerID = 1;
  touch_down.tfinger.x = 0.5f;
  touch_down.tfinger.y = 0.5f;
  touch_down.tfinger.pressure = 1.0f;

  SDL_Event touch_up = touch_down;
  touch_up.type = SDL_EVENT_FINGER_UP;
  touch_up.tfinger.pressure = 0.0f;

  g_state.input_test_sequence_queued = true;
  if ((needs_focus && !SDL_PushEvent(&focus)) || !SDL_PushEvent(&motion) ||
      !SDL_PushEvent(&down) || !SDL_PushEvent(&up) ||
      !SDL_PushEvent(&touch_down) || !SDL_PushEvent(&touch_up)) {
    fprintf(stderr,
            "  [input] failed to queue SDL readiness pointer sequence: %s\n",
            SDL_GetError());
    return;
  }
  fprintf(stderr, "  [input] SDL readiness pointer sequence queued\n");
}

void MaybeRequestResizeReadiness() {
  if (g_resize_readiness_gate == nullptr || g_state.sdl_window == nullptr) {
    return;
  }
  WindowResizeRequest request;
  if (!g_resize_readiness_gate->TakeResizeRequest(&request)) {
    return;
  }
  if (g_state.resize_target_logical_width <= 0 ||
      g_state.resize_target_logical_height <= 0 ||
      !SDL_SetWindowSize(g_state.sdl_window,
                         g_state.resize_target_logical_width,
                         g_state.resize_target_logical_height)) {
    fprintf(stderr, "  [resize] SDL_SetWindowSize failed: %s\n",
            SDL_GetError());
    return;
  }
  const WindowResizeReadinessSnapshot snapshot =
      g_resize_readiness_gate->Snapshot();
  fprintf(stderr,
          "  [resize] SDL compositor resize requested after present=%llu "
          "logical=%dx%d expected_pixels=%ux%u\n",
          static_cast<unsigned long long>(snapshot.first_present_serial),
          g_state.resize_target_logical_width,
          g_state.resize_target_logical_height, request.width, request.height);
}

bool FullscreenShortcutEnabled() {
  const char* value = GetEnvNonEmpty("BLOX7_F11_FULLSCREEN");
  return value == nullptr || !StringEquals(value, "0");
}

bool RequestFullscreenState(bool fullscreen, const char* reason) {
  if (g_state.sdl_window == nullptr) {
    return false;
  }
  const bool current_fullscreen =
      (SDL_GetWindowFlags(g_state.sdl_window) & SDL_WINDOW_FULLSCREEN) != 0;
  if (current_fullscreen != fullscreen) {
    CaptureWindowState();
    g_fullscreen_switching.store(true, std::memory_order_release);
    const bool switched =
        SDL_SetWindowFullscreen(g_state.sdl_window, fullscreen);
    g_fullscreen_switching.store(false, std::memory_order_release);
    if (!switched) {
      fprintf(stderr, "  [fullscreen] SDL request failed: %s\n",
              SDL_GetError());
      return false;
    }
    fprintf(stderr, "  [fullscreen] %s requested state=%s\n",
            reason != nullptr ? reason : "toggle",
            fullscreen ? "fullscreen" : "windowed");
  }
  if (g_state.state_persistence_active) {
    g_state.persisted_window.fullscreen = fullscreen;
    g_state.state_persistence_dirty = true;
    g_state.state_persistence_change_ticks_ns = SDL_GetTicksNS();
    const Status store_status =
        StoreWindowState(g_window_state_path, g_state.persisted_window);
    if (!store_status.ok()) {
      std::fprintf(stderr, "  [window-state] fullscreen save failed: %s\n",
                   store_status.message().c_str());
    } else {
      g_state.state_persistence_dirty = false;
    }
  }
  if (!g_fullscreen_state_sync.Notify(fullscreen)) {
    fprintf(stderr,
            "  [fullscreen] Roblox settings state synchronization failed\n");
    return false;
  }
  return true;
}

bool RequestFullscreenToggle(const char* reason) {
  if (g_state.sdl_window == nullptr) {
    return false;
  }
  const bool fullscreen =
      (SDL_GetWindowFlags(g_state.sdl_window) & SDL_WINDOW_FULLSCREEN) != 0;
  return RequestFullscreenState(!fullscreen, reason);
}

void MaybeApplyAndroidFullscreenRequest() {
  bool fullscreen = false;
  if (!g_fullscreen_request_gate.Take(&fullscreen)) {
    return;
  }
  if (!RequestFullscreenState(fullscreen, "Roblox settings")) {
    fprintf(stderr, "  [fullscreen] rejected Android setWindowFlags request\n");
  }
}

void MaybeApplyRobloxFullscreenMenuRequest() {
  if (!g_fullscreen_menu_request_gate.Take(SDL_GetTicksNS())) {
    return;
  }
  if (!RequestFullscreenToggle("Roblox menu")) {
    fprintf(stderr, "  [fullscreen] rejected Roblox menu toggle request\n");
  }
}

bool HandleFullscreenShortcut(const SDL_Event& event) {
  if (!FullscreenShortcutEnabled() ||
      (event.type != SDL_EVENT_KEY_DOWN && event.type != SDL_EVENT_KEY_UP) ||
      event.key.scancode != SDL_SCANCODE_F11) {
    return false;
  }
  if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) {
    (void)RequestFullscreenToggle("F11");
  }
  return true;
}

void MaybeRequestFullscreenReadiness() {
  static const char* const mode = GetEnvNonEmpty("BLOX7_FULLSCREEN_READINESS");
  const bool single_transition = StringEquals(mode, "1");
  const bool round_trip = StringEquals(mode, "roundtrip");
  if ((!single_transition && !round_trip) || !HasPresentedFrame()) {
    return;
  }
  const int present_count = g_real_swap_count.load(std::memory_order_acquire);
  if (!g_state.fullscreen_readiness_requested) {
    g_state.fullscreen_readiness_requested = true;
    g_state.fullscreen_readiness_present_baseline = present_count;
    if (!RequestFullscreenToggle("readiness")) {
      g_state.quit_requested = true;
    }
    return;
  }
  constexpr int kRoundTripStablePresents = 60;
  const bool currently_fullscreen =
      g_state.sdl_window != nullptr &&
      (SDL_GetWindowFlags(g_state.sdl_window) & SDL_WINDOW_FULLSCREEN) != 0;
  if (!round_trip || g_state.fullscreen_readiness_return_requested ||
      !currently_fullscreen ||
      present_count - g_state.fullscreen_readiness_present_baseline <
          kRoundTripStablePresents) {
    return;
  }
  g_state.fullscreen_readiness_return_requested = true;
  if (!RequestFullscreenToggle("readiness-return")) {
    g_state.quit_requested = true;
  }
}

void MaybeRecoverVulkanSurface() {
  if (!g_state.direct_vulkan || !g_window_surface_lifecycle.active()) {
    return;
  }
  VulkanSurfaceRecoveryRequest request;
  if (!g_vulkan_surface_recovery_gate.TakeRebindRequest(SDL_GetTicksNS(),
                                                        &request)) {
    return;
  }
  const Status recreate_status = g_window_surface_lifecycle.Recreate();
  if (!recreate_status.ok()) {
    fprintf(stderr, "  [vulkan] surface recovery rejected: %s\n",
            recreate_status.message().c_str());
    g_state.quit_requested = true;
    return;
  }
  const WindowSurfaceSnapshot snapshot = g_window_surface_lifecycle.Snapshot();
  fprintf(stderr,
          "  [vulkan] swapchain out of date count=%llu coalesced=%llu; "
          "queued typed surface %s recreation generation=%llu pixels=%ux%u\n",
          static_cast<unsigned long long>(request.observed_error_count),
          static_cast<unsigned long long>(request.coalesced_error_count),
          request.retry ? "retry" : "recovery",
          static_cast<unsigned long long>(snapshot.generation), snapshot.width,
          snapshot.height);
}

void MaybeReportVulkanPresentStall() {
  if (!g_state.direct_vulkan || g_state.sdl_window == nullptr) {
    return;
  }
  const SDL_WindowFlags flags = SDL_GetWindowFlags(g_state.sdl_window);
  const bool window_can_present =
      (flags & (SDL_WINDOW_HIDDEN | SDL_WINDOW_MINIMIZED)) == 0;
  VulkanPresentStallReport report;
  if (!g_vulkan_present_progress_gate.Poll(SDL_GetTicksNS(), window_can_present,
                                           &report)) {
    return;
  }
  const uint64_t now_ticks_ns = SDL_GetTicksNS();
  const uint64_t completed_ago_ms =
      report.last_completed_call_completed_ns != 0 &&
              now_ticks_ns >= report.last_completed_call_completed_ns
          ? (now_ticks_ns - report.last_completed_call_completed_ns) /
                1000000ULL
          : 0;
  const uint64_t completed_duration_ms =
      report.last_completed_call_completed_ns >=
                  report.last_completed_call_started_ns &&
              report.last_completed_call_started_ns != 0
          ? (report.last_completed_call_completed_ns -
             report.last_completed_call_started_ns) /
                1000000ULL
          : 0;
  const char* stage = "before_host_present";
  if (report.stage == VulkanPresentStallStage::kInsideVulkanCall) {
    stage = "inside_vulkan_call";
  } else if (report.stage == VulkanPresentStallStage::kInsideHostPresent) {
    stage = "inside_host_present";
  }
  fprintf(
      stderr,
      "  [vulkan] progress watchdog: stage=%s stalled_for=%llu ms "
      "frame=%llu host_presents=%llu render_tid=%llu primary_call=%s "
      "primary_id=%llu primary_tid=%llu last_snapshot=%s last_call=%s "
      "last_id=%llu last_tid=%llu last_result=%d last_duration=%llu ms "
      "completed_ago=%llu ms secondary_call=%s secondary_id=%llu "
      "secondary_tid=%llu secondary_for=%llu ms\n",
      stage, static_cast<unsigned long long>(report.elapsed_ns / 1000000ULL),
      static_cast<unsigned long long>(report.frame_count),
      static_cast<unsigned long long>(report.host_present_count),
      static_cast<unsigned long long>(report.last_present_thread_id),
      report.call_name != nullptr ? report.call_name : "none",
      static_cast<unsigned long long>(report.call_sequence),
      static_cast<unsigned long long>(report.thread_id),
      report.last_completed_call_snapshot_coherent ? "coherent" : "unavailable",
      report.last_completed_call_name != nullptr
          ? report.last_completed_call_name
          : "none",
      static_cast<unsigned long long>(report.last_completed_call_sequence),
      static_cast<unsigned long long>(report.last_completed_call_thread_id),
      report.last_completed_call_result,
      static_cast<unsigned long long>(completed_duration_ms),
      static_cast<unsigned long long>(completed_ago_ms),
      report.oldest_active_call_name != nullptr ? report.oldest_active_call_name
                                                : "none",
      static_cast<unsigned long long>(report.oldest_active_call_sequence),
      static_cast<unsigned long long>(report.oldest_active_call_thread_id),
      static_cast<unsigned long long>(report.oldest_active_call_elapsed_ns /
                                      1000000ULL));
}

// a wheel notch eases in over a few frames like the windows client, BLOX7_SMOOTH_ZOOM=0 for the old jump
struct SmoothWheel {
  float left = 0, x = 0, y = 0;
  bool relative = false;
  uint64_t last_ns = 0;
};
static SmoothWheel g_wheel;

static bool SmoothWheelEnabled() {
  static const bool enabled = [] {
    const char* value = GetEnvNonEmpty("BLOX7_SMOOTH_ZOOM");
    return value == nullptr || std::strcmp(value, "0") != 0;
  }();
  return enabled;
}

static void EmitSmoothWheel() {
  if (g_wheel.left == 0.0f) {
    g_wheel.last_ns = 0;
    return;
  }
  const uint64_t now = SDL_GetTicksNS();
  const float dt = g_wheel.last_ns != 0 ? static_cast<float>(now - g_wheel.last_ns) / 1e9f : 1.0f / 60;
  g_wheel.last_ns = now;
  float step = g_wheel.left * (1.0f - std::exp(-dt / 0.07f));
  if (std::fabs(g_wheel.left - step) < 0.02f) step = g_wheel.left;
  g_wheel.left -= step;
  platform::PlatformEvent event;
  event.timestamp_ns = now;
  event.payload = platform::MouseWheelEvent{0.0f, step, g_wheel.x, g_wheel.y, g_wheel.relative};
  g_platform_event_observer.Notify(event);
}

static void NotifyPointerWarp() {
  if (g_pointer_capture_owner == nullptr) return;
  const std::optional<PointerPosition> warp = g_pointer_capture_owner->TakeWarp();
  if (!warp) return;
  platform::PlatformEvent event;
  event.timestamp_ns = SDL_GetTicksNS();
  event.payload = platform::MouseMotionEvent{warp->x, warp->y, 0.0f, 0.0f, SDL_GetMouseState(nullptr, nullptr), false};
  g_platform_event_observer.Notify(event);
}

bool PumpEvents() {
  g_pump_thread_id.store(SDL_GetCurrentThreadID(), std::memory_order_release);
  if (g_show_requested.exchange(false, std::memory_order_acq_rel)) {
    ShowIfHidden();
  }
  MaybeSynchronizeRestoredFullscreenState();
  MaybeApplyRobloxFullscreenMenuRequest();
  MaybeApplyAndroidFullscreenRequest();
  MaybeRequestResizeReadiness();
  MaybeRequestFullscreenReadiness();
  MaybeQueueInputReadinessSequence();
  if (!g_pre_text_input_pump_gate.Invoke()) {
    fprintf(stderr, "  [input] main-thread text command drain failed\n");
  }
  if (g_text_input_owner != nullptr && !g_text_input_owner->Pump()) {
    fprintf(stderr, "  [input] SDL text-input state update failed\n");
  }
  if (g_pointer_capture_owner != nullptr &&
      !g_pointer_capture_owner->Pump(g_text_input_owner != nullptr &&
                                     g_text_input_owner->active())) {
    fprintf(stderr, "  [input] SDL pointer capture state update failed\n");
  }
  NotifyPointerWarp();
  platform::PlatformEvent pending_motion{};
  bool has_pending_motion = false;

  auto flush_motion = [&]() {
    if (has_pending_motion) {
      g_platform_event_observer.Notify(pending_motion);
      has_pending_motion = false;
      pending_motion = {};
    }
  };

  SDL_Event event;
  bool pump_os_events = true;
  pump_os_events = !g_in_live_resize_tick;
  if (pump_os_events) {
    SDL_PumpEvents();
  }
  while (SDL_PeepEvents(&event, 1, SDL_GETEVENT, SDL_EVENT_FIRST,
                        SDL_EVENT_LAST) > 0) {
    if (event.type != SDL_EVENT_MOUSE_MOTION) {
      flush_motion();
    }
    const bool is_window_event = event.type >= SDL_EVENT_WINDOW_FIRST &&
                                 event.type <= SDL_EVENT_WINDOW_LAST;
    if (is_window_event &&
        event.window.windowID != SDL_GetWindowID(g_state.sdl_window)) {
      continue;
    }
    if (event.type == SDL_EVENT_QUIT) {
      if (!ShouldIgnoreCloseRequest()) {
        g_state.quit_requested = true;
      }
    }
    if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
      if (!ShouldIgnoreCloseRequest()) {
        g_state.quit_requested = true;
      }
    }
    if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST &&
        g_text_input_owner != nullptr) {
      g_text_input_owner->OnFocusLost();
    }
    if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST &&
        g_pointer_capture_owner != nullptr &&
        !g_pointer_capture_owner->OnFocusLost()) {
      fprintf(stderr, "  [input] failed to release pointer after focus loss\n");
    }
    if (event.type == SDL_EVENT_WINDOW_FOCUS_GAINED &&
        g_text_input_owner != nullptr) {
      g_text_input_owner->OnFocusGained();
    }
    if (event.type == SDL_EVENT_WINDOW_FOCUS_GAINED &&
        g_pointer_capture_owner != nullptr &&
        !g_pointer_capture_owner->OnFocusGained(
            g_text_input_owner != nullptr && g_text_input_owner->active())) {
      fprintf(stderr, "  [input] failed to restore pointer after focus gain\n");
    }
    NotifyPointerWarp();

    const bool geometry_changed = event.type == SDL_EVENT_WINDOW_MOVED ||
                                  event.type == SDL_EVENT_WINDOW_RESIZED ||
                                  event.type == SDL_EVENT_WINDOW_MAXIMIZED ||
                                  event.type == SDL_EVENT_WINDOW_RESTORED;
    const bool fullscreen_changed =
        event.type == SDL_EVENT_WINDOW_ENTER_FULLSCREEN ||
        event.type == SDL_EVENT_WINDOW_LEAVE_FULLSCREEN;
    if (geometry_changed || fullscreen_changed) {
      CaptureWindowState();
      if (fullscreen_changed) {
        g_state.persisted_window.fullscreen =
            event.type == SDL_EVENT_WINDOW_ENTER_FULLSCREEN;
      }
      MarkWindowStateDirty(fullscreen_changed);
    }

    const bool fullscreen_shortcut = HandleFullscreenShortcut(event);
    platform::PlatformEvent platform_event;
    const bool converted =
        platform::ConvertSdlEvent(
            g_state.sdl_window, event, &platform_event,
            SDL_GetWindowRelativeMouseMode(g_state.sdl_window));
    if (is_window_event && g_window_surface_lifecycle.active()) {
      int pixel_width = 0;
      int pixel_height = 0;
      SDL_GetWindowSizeInPixels(g_state.sdl_window, &pixel_width,
                                &pixel_height);
      void* observed_native_window = QueryNativeWindowHandle();
      const WindowSurfaceSnapshot previous_surface =
          g_window_surface_lifecycle.Snapshot();
      const Status surface_status = g_window_surface_lifecycle.Observe(
          reinterpret_cast<uintptr_t>(observed_native_window),
          pixel_width > 0 ? static_cast<uint32_t>(pixel_width) : 0,
          pixel_height > 0 ? static_cast<uint32_t>(pixel_height) : 0,
          QueryWindowDpiScale());
      if (!surface_status.ok()) {
        fprintf(stderr, "  [window] surface event rejected: %s\n",
                surface_status.message().c_str());
        g_state.quit_requested = true;
      } else {
        const bool fullscreen_transition =
            event.type == SDL_EVENT_WINDOW_ENTER_FULLSCREEN ||
            event.type == SDL_EVENT_WINDOW_LEAVE_FULLSCREEN;
        if (fullscreen_transition && pixel_width > 0 && pixel_height > 0) {
          const Status refresh_status = g_window_surface_lifecycle.Refresh();
          if (!refresh_status.ok()) {
            fprintf(stderr, "  [fullscreen] surface refresh rejected: %s\n",
                    refresh_status.message().c_str());
            g_state.quit_requested = true;
          } else {
            fprintf(stderr,
                    "  [fullscreen] SDL transition committed state=%s "
                    "pixels=%dx%d\n",
                    event.type == SDL_EVENT_WINDOW_ENTER_FULLSCREEN
                        ? "fullscreen"
                        : "windowed",
                    pixel_width, pixel_height);
          }
        }
        g_state.native_window = observed_native_window;
        const WindowSurfaceSnapshot surface =
            g_window_surface_lifecycle.Snapshot();
        if (previous_surface.available && surface.available &&
            (previous_surface.width != surface.width ||
             previous_surface.height != surface.height)) {
          g_vulkan_surface_recovery_gate.NotifyResize(SDL_GetTicksNS());
        }
        if (surface.available) {
          g_state.width = static_cast<int>(surface.width);
          g_state.height = static_cast<int>(surface.height);
        }
        if ((event.type == SDL_EVENT_WINDOW_RESIZED ||
             event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED ||
             event.type == SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED ||
             event.type == SDL_EVENT_WINDOW_ENTER_FULLSCREEN ||
             event.type == SDL_EVENT_WINDOW_LEAVE_FULLSCREEN) &&
            g_text_input_owner != nullptr &&
            !g_text_input_owner->OnViewportChanged()) {
          fprintf(
              stderr,
              "  [input] SDL text-input area reapply failed after resize\n");
        }
      }
    }
    if (converted && !fullscreen_shortcut) {
      if (event.type == SDL_EVENT_MOUSE_MOTION) {
        if (!has_pending_motion) {
          pending_motion = platform_event;
          has_pending_motion = true;
        } else {
          auto* cur =
              std::get_if<platform::MouseMotionEvent>(&pending_motion.payload);
          const auto* nxt =
              std::get_if<platform::MouseMotionEvent>(&platform_event.payload);
          if (cur != nullptr && nxt != nullptr &&
              cur->relative_mode == nxt->relative_mode) {
            cur->delta_x += nxt->delta_x;
            cur->delta_y += nxt->delta_y;
            cur->x = nxt->x;
            cur->y = nxt->y;
            cur->buttons = nxt->buttons;
            pending_motion.timestamp_ns = platform_event.timestamp_ns;
          } else {
            flush_motion();
            pending_motion = platform_event;
            has_pending_motion = true;
          }
        }
      } else if (auto* wheel = std::get_if<platform::MouseWheelEvent>(&platform_event.payload);
                 wheel != nullptr && wheel->delta_y != 0.0f && SmoothWheelEnabled()) {
        g_wheel.left += wheel->delta_y;
        g_wheel.x = wheel->mouse_x;
        g_wheel.y = wheel->mouse_y;
        g_wheel.relative = wheel->relative_mode;
        if (wheel->delta_x != 0.0f) {
          wheel->delta_y = 0.0f;
          g_platform_event_observer.Notify(platform_event);
        }
      } else {
        g_platform_event_observer.Notify(platform_event);
      }
    }
    if (converted && !fullscreen_shortcut && event.type == SDL_EVENT_KEY_DOWN &&
        !event.key.repeat &&
        (event.key.scancode == SDL_SCANCODE_LSHIFT ||
         event.key.scancode == SDL_SCANCODE_RSHIFT) &&
        g_pointer_capture_owner != nullptr &&
        !g_pointer_capture_owner->OnShiftKeyPressed(
            g_text_input_owner != nullptr && g_text_input_owner->active())) {
      fprintf(stderr,
              "  [input] SDL shift-lock pointer capture update failed\n");
    }
    NotifyPointerWarp();
    if ((event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
         event.type == SDL_EVENT_MOUSE_BUTTON_UP) &&
        event.button.button == SDL_BUTTON_RIGHT &&
        event.button.windowID == SDL_GetWindowID(g_state.sdl_window) &&
        g_pointer_capture_owner != nullptr &&
        !g_pointer_capture_owner->OnRightButton(
            event.button.down,
            g_text_input_owner != nullptr && g_text_input_owner->active(),
            PointerPosition{event.button.x, event.button.y})) {
      fprintf(stderr,
              "  [input] SDL right-button pointer capture update failed\n");
    }
    NotifyPointerWarp();
  }
  flush_motion();
  EmitSmoothWheel();
  UpdateWindowTextLayer();
  MaybePersistWindowState();
  MaybeRecoverVulkanSurface();
  MaybeReportVulkanPresentStall();
  static const char* auto_exit_value =
      GetEnvNonEmpty("BLOX7_AUTO_EXIT_AFTER_PRESENT_MS");
  if (!g_state.quit_requested && auto_exit_value != nullptr) {
    char* end = nullptr;
    const long delay_ms = std::strtol(auto_exit_value, &end, 10);
    const uint64_t first_present_ns =
        g_first_present_ticks_ns.load(std::memory_order_acquire);
    if (end != auto_exit_value && delay_ms > 0 && first_present_ns != 0) {
      const uint64_t elapsed_ns = SDL_GetTicksNS() - first_present_ns;
      if (elapsed_ns >= static_cast<uint64_t>(delay_ms) * 1000000ULL) {
        fprintf(stderr,
                "  [window] automatic exit requested %ld ms after first "
                "present\n",
                delay_ms);
        g_state.quit_requested = true;
      }
    }
  }
  PublishPresentWindow();
  return !g_state.quit_requested;
}

bool UnthrottledPresentationRequested() {
  const auto policy = graphics::CachedPresentModePolicy();
  return policy == graphics::PresentModePolicy::kUnthrottled ||
         policy == graphics::PresentModePolicy::kImmediate;
}

uint64_t PaceInputPump() {
  if (!g_state.initialised) {
    return 0;
  }
  if (__builtin_expect(SDL_HasEvents(SDL_EVENT_FIRST, SDL_EVENT_LAST), 0)) {
    return 0;
  }
  const uint64_t delay_ns =
      g_input_pump_pacer.DelayBeforeNextPump(SDL_GetTicksNS());
  if (delay_ns != 0) {
    SDL_WaitEventTimeout(nullptr, static_cast<Sint32>((delay_ns + 999999) / 1000000));
  }
  return delay_ns;
}

void Shutdown() {
  g_input_pump_pacer.Reset();
  g_pre_text_input_pump_gate.Deactivate();
  g_platform_event_observer.Deactivate();
  g_vulkan_present_progress_gate.Deactivate();
  g_vulkan_surface_recovery_gate.Deactivate();
  g_window_surface_lifecycle.Deactivate();
  g_present_lifecycle.Deactivate();
  if (!g_state.initialised) {
    g_fullscreen_request_gate.Reset();
    g_fullscreen_menu_request_gate.Reset();
    g_pointer_capture_owner.reset();
    g_pointer_capture_backend.reset();
    g_text_input_owner.reset();
    g_text_input_backend.reset();
    return;
  }
  if (WindowTraceEnabled()) {
    fprintf(stderr, "  [window] Shutdown window=%p context=%p surface=%p\n",
            static_cast<void*>(g_state.sdl_window), g_state.egl_context,
            g_state.egl_surface);
  }
  (void)PersistWindowState();
  if (g_text_input_owner != nullptr) {
    g_text_input_owner->Shutdown();
  }
  if (g_pointer_capture_owner != nullptr &&
      !g_pointer_capture_owner->Shutdown()) {
    fprintf(stderr, "  [input] SDL pointer capture shutdown failed\n");
  }
  g_pointer_capture_owner.reset();
  g_pointer_capture_backend.reset();
  g_text_input_owner.reset();
  g_text_input_backend.reset();
  g_gles_text_overlay.reset();
  ::blox7::win32::DestroyTextLayer();
  g_present_window.store(nullptr, std::memory_order_release);
  if (!g_state.direct_vulkan && g_state.egl_context != nullptr) {
    SDL_GL_MakeCurrent(g_state.sdl_window, nullptr);
  }
  if (!g_state.direct_vulkan && g_state.egl_context) {
    SDL_GL_DestroyContext(static_cast<SDL_GLContext>(g_state.egl_context));
    g_state.egl_context = nullptr;
  }
  if (g_state.sdl_window) {
    SDL_DestroyWindow(g_state.sdl_window);
    g_state.sdl_window = nullptr;
  }
  SDL_Quit();
  g_state.initialised = false;
  g_state.native_window = nullptr;
  g_state.egl_swap_interval = nullptr;
  g_state.min_swap_interval = 1;
  g_state.max_swap_interval = 1;
  g_state.direct_vulkan = false;
  g_state.display_refresh = {};
  g_state.input_test_sequence_queued = false;
  g_state.resize_target_logical_width = 0;
  g_state.resize_target_logical_height = 0;
  g_state.fullscreen_readiness_requested = false;
  g_state.fullscreen_readiness_return_requested = false;
  g_state.restored_fullscreen_sync_pending = false;
  g_state.fullscreen_readiness_present_baseline = 0;
  g_state.state_persistence_active = false;
  g_state.state_persistence_dirty = false;
  g_state.state_persistence_change_ticks_ns = 0;
  g_state.persisted_window = {};
  g_fullscreen_request_gate.Reset();
  g_fullscreen_menu_request_gate.Reset();
  g_real_swap_count.store(0, std::memory_order_relaxed);
  g_first_present_ticks_ns.store(0, std::memory_order_relaxed);
  g_resize_readiness_gate.reset();
}

bool RequestFullscreenFromAndroidWindowFlags(int flags, int mask) {
  return g_fullscreen_request_gate.RequestFromAndroidFlags(flags, mask);
}

bool RequestFullscreenFromRobloxMenuLog(const char* tag, const char* message) {
  return g_fullscreen_menu_request_gate.RequestFromAndroidLog(tag, message,
                                                              SDL_GetTicksNS());
}

bool SetFullscreenStateSyncCallback(FullscreenStateSyncCallback callback,
                                    void* context) {
  return g_fullscreen_state_sync.Register(callback, context);
}

void ClearFullscreenStateSyncCallback() { g_fullscreen_state_sync.Clear(); }

}  // namespace window
}  // namespace blox7

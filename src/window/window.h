#ifndef BLOX7_WINDOW_WINDOW_H_
#define BLOX7_WINDOW_WINDOW_H_

#include <cstdint>
#include <filesystem>

#include "blox7/platform/display_refresh_capabilities.h"
#include "blox7/status.h"
#include "window/platform_event_observer.h"
#include "window/present_observer.h"
#include "window/window_pointer_capture_owner.h"
#include "window/window_surface_lifecycle.h"
#include "window/window_text_input_owner.h"

namespace blox7 {
namespace window {

using PreTextInputPumpCallback = bool (*)(void* context);
using FullscreenStateSyncCallback = bool (*)(void* context, bool fullscreen);

Status ConfigureWindowStatePersistence(const std::filesystem::path& path);

bool Init(int width, int height, const char* title);

bool PumpEvents();
void SetLiveResizeTick(void (*tick)(void*), void* context);

uint64_t PaceInputPump();

bool UnthrottledPresentationRequested();

bool RequestFullscreenFromAndroidWindowFlags(int flags, int mask);

bool RequestFullscreenFromRobloxMenuLog(const char* tag, const char* message);

bool SetFullscreenStateSyncCallback(FullscreenStateSyncCallback callback,
                                    void* context);
void ClearFullscreenStateSyncCallback();

bool PollWindowSurfaceEvent(WindowSurfaceEvent* event);
WindowSurfaceSnapshot GetWindowSurfaceSnapshot();

Status RecordResizeReadinessSurfaceCommit(const WindowSurfaceEvent& event);

Status StopResizeReadiness();
Status ResizeReadinessCompletionStatus();

struct WindowViewportSnapshot {
  int logical_width = 0;
  int logical_height = 0;
  int pixel_width = 0;
  int pixel_height = 0;
  float dpi_scale = 1.0f;

  bool valid() const {
    return logical_width > 0 && logical_height > 0 && pixel_width > 0 &&
           pixel_height > 0;
  }
};

WindowViewportSnapshot GetWindowViewportSnapshot();

platform::DisplayRefreshCapabilities GetDisplayRefreshCapabilities();

bool SetPlatformEventObserver(PlatformEventObserver observer, void* context);
void ClearPlatformEventObserver();

bool SetMouseLockQueryCallback(MouseLockQueryCallback callback, void* context);
void ClearMouseLockQueryCallback();

bool SetPreTextInputPumpCallback(PreTextInputPumpCallback callback,
                                 void* context);
void ClearPreTextInputPumpCallback();

void SetWindowTextInputOwnerEnabled(bool enabled);
bool RequestShowTextInput(uint64_t generation, const TextInputArea& area,
                          const TextInputOptions& options);
bool RequestHideTextInput(uint64_t generation);

void* GetEGLDisplay();

void* GetEGLSurface();

void* GetEGLContext();

void* GetEGLConfig();

void* GetNativeWindow();

void* GetBackendWindow();

bool UsesDirectVulkan();

void* GetGLProcAddress(const char* name);

bool SwapBuffers();
bool SetSwapInterval(int interval);

bool HasPresentedFrame();

void NoteVulkanPresent();

void NoteVulkanSurfaceOutOfDate();

bool SetPresentObserver(PresentObserver observer, void* context);
void ClearPresentObserver();

class ScopedPresentObserver final {
 public:
  ScopedPresentObserver() = default;
  ~ScopedPresentObserver();

  ScopedPresentObserver(const ScopedPresentObserver&) = delete;
  ScopedPresentObserver& operator=(const ScopedPresentObserver&) = delete;

  bool Register(PresentObserver observer, void* context);
  void Reset();

 private:
  bool registered_ = false;
};

void ShowIfHidden();

bool MakeCurrentOnThread();

bool ReleaseCurrentOnThread();

void Shutdown();

bool IsInitialised();

int GetWidth();
int GetHeight();

}  // namespace window
}  // namespace blox7

#endif  // BLOX7_WINDOW_WINDOW_H_

#include <cstdint>

extern "C" void* __w32_attach_thread();
extern "C" void* __w32_pe_open(const char* utf8_path);
extern "C" void* __w32_pe_symbol(void* module, const char* name);

namespace blox7::win32 {
namespace {

#define MS __attribute__((ms_abi))

using Handle = void*;

struct Rect {
  int32_t left, top, right, bottom;
};
struct WindowPos {
  Handle window, insert_after;
  int32_t x, y, cx, cy;
  uint32_t flags;
};
struct MonitorInfo {
  uint32_t size;
  Rect monitor, work;
  uint32_t flags;
};
static_assert(sizeof(WindowPos) == 40, "WINDOWPOS");
static_assert(sizeof(MonitorInfo) == 40, "MONITORINFO");

constexpr uint32_t kWindowPosChanging = 0x0046;
constexpr uint32_t kWindowPosChanged = 0x0047;
constexpr uint32_t kNcDestroy = 0x0082;
constexpr uint32_t kNoSize = 0x0001;
constexpr uint32_t kNoMove = 0x0002;
constexpr uint32_t kNoZOrder = 0x0004;
constexpr uint32_t kNoActivate = 0x0010;
constexpr uint32_t kStyleCaption = 0x00C00000;
constexpr int32_t kWindowProc = -4;
constexpr int32_t kStyle = -16;
constexpr int32_t kHiddenRows = 1;

struct Api {
  intptr_t(MS* CallWindowProcW)(void*, Handle, uint32_t, uintptr_t, intptr_t);
  intptr_t(MS* GetWindowLongPtrW)(Handle, int32_t);
  intptr_t(MS* SetWindowLongPtrW)(Handle, int32_t, intptr_t);
  int32_t(MS* GetWindowRect)(Handle, Rect*);
  int32_t(MS* SetWindowPos)(Handle, Handle, int32_t, int32_t, int32_t, int32_t, uint32_t);
  int32_t(MS* SetWindowRgn)(Handle, Handle, int32_t);
  Handle(MS* MonitorFromRect)(const Rect*, uint32_t);
  int32_t(MS* GetMonitorInfoW)(Handle, MonitorInfo*);
  Handle(MS* CreateRectRgn)(int32_t, int32_t, int32_t, int32_t);

  template <typename T>
  static bool Find(void* module, const char* name, T* slot) {
    *slot = reinterpret_cast<T>(__w32_pe_symbol(module, name));
    return *slot != nullptr;
  }

  bool Load() {
    void* user32 = __w32_pe_open("user32.dll");
    void* gdi32 = __w32_pe_open("gdi32.dll");
    return user32 != nullptr && gdi32 != nullptr && Find(user32, "CallWindowProcW", &CallWindowProcW) &&
           Find(user32, "GetWindowLongPtrW", &GetWindowLongPtrW) &&
           Find(user32, "SetWindowLongPtrW", &SetWindowLongPtrW) &&
           Find(user32, "GetWindowRect", &GetWindowRect) && Find(user32, "SetWindowPos", &SetWindowPos) &&
           Find(user32, "SetWindowRgn", &SetWindowRgn) && Find(user32, "MonitorFromRect", &MonitorFromRect) &&
           Find(user32, "GetMonitorInfoW", &GetMonitorInfoW) && Find(gdi32, "CreateRectRgn", &CreateRectRgn);
  }
};

Api g_api;
void* g_game_proc = nullptr;
bool g_clipped = false;

bool CoversAMonitor(Handle window, const Rect& rect) {
  if ((static_cast<uint32_t>(g_api.GetWindowLongPtrW(window, kStyle)) & kStyleCaption) != 0) {
    return false;
  }
  MonitorInfo info{};
  info.size = sizeof info;
  Handle monitor = g_api.MonitorFromRect(&rect, 0);
  return monitor != nullptr && g_api.GetMonitorInfoW(monitor, &info) != 0 && rect.left == info.monitor.left &&
         rect.top == info.monitor.top && rect.right == info.monitor.right && rect.bottom == info.monitor.bottom;
}

__attribute__((noinline)) intptr_t Positioned(Handle window, uint32_t message, uintptr_t wparam,
                                              intptr_t lparam) {
  const intptr_t result = g_api.CallWindowProcW(g_game_proc, window, message, wparam, lparam);
  Rect now{};
  if (!g_api.GetWindowRect(window, &now)) {
    return result;
  }
  if (message == kWindowPosChanging) {
    WindowPos* next = reinterpret_cast<WindowPos*>(lparam);
    Rect wanted{};
    wanted.left = (next->flags & kNoMove) != 0 ? now.left : next->x;
    wanted.top = (next->flags & kNoMove) != 0 ? now.top : next->y;
    wanted.right = wanted.left + ((next->flags & kNoSize) != 0 ? now.right - now.left : next->cx);
    wanted.bottom = wanted.top + ((next->flags & kNoSize) != 0 ? now.bottom - now.top : next->cy);
    if (CoversAMonitor(window, wanted)) {
      next->cx = wanted.right - wanted.left;
      next->cy = wanted.bottom - wanted.top + kHiddenRows;
      next->flags &= ~kNoSize;
    }
    return result;
  }
  Rect shown = now;
  shown.bottom -= kHiddenRows;
  const bool clip = CoversAMonitor(window, shown);
  if (clip != g_clipped) {
    g_clipped = clip;
    g_api.SetWindowRgn(window,
                       clip ? g_api.CreateRectRgn(0, 0, shown.right - shown.left, shown.bottom - shown.top)
                            : nullptr,
                       1);
  }
  return result;
}

MS intptr_t GameWindowProc(Handle window, uint32_t message, uintptr_t wparam, intptr_t lparam) {
  if (message == kWindowPosChanging || message == kWindowPosChanged) {
    __w32_attach_thread();
    return Positioned(window, message, wparam, lparam);
  }
  void* game_proc = g_game_proc;
  if (message == kNcDestroy) {
    g_game_proc = nullptr;
    g_clipped = false;
  }
  return g_api.CallWindowProcW(game_proc, window, message, wparam, lparam);
}

}  // namespace

bool KeepFullscreenComposited(void* game_window) {
  static const bool loaded = g_api.Load();
  if (!loaded || game_window == nullptr || g_game_proc != nullptr) {
    return false;
  }
  g_game_proc = reinterpret_cast<void*>(g_api.GetWindowLongPtrW(game_window, kWindowProc));
  if (g_game_proc == nullptr) {
    return false;
  }
  g_api.SetWindowLongPtrW(game_window, kWindowProc, reinterpret_cast<intptr_t>(&GameWindowProc));
  g_api.SetWindowPos(game_window, nullptr, 0, 0, 0, 0, kNoMove | kNoSize | kNoZOrder | kNoActivate);
  return true;
}

}  // namespace blox7::win32

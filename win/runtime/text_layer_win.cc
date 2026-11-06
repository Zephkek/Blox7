// the focused TextBox floats over the game in its own window so typing shows up even at 1 fps
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "blox7/graphics/text_overlay_frame.h"

extern "C" void* __w32_pe_open(const char* utf8_path);
extern "C" void* __w32_pe_symbol(void* module, const char* name);

namespace blox7::win32 {
namespace {

#define MS __attribute__((ms_abi))

using Handle = void*;

struct Point {
  int32_t x, y;
};
struct Size {
  int32_t cx, cy;
};
struct Rect {
  int32_t left, top, right, bottom;
};
struct BlendFunction {
  uint8_t operation, flags, constant_alpha, alpha_format;
};
struct BitmapInfo {
  uint32_t size;
  int32_t width, height;
  uint16_t planes, bit_count;
  uint32_t compression, image_size;
  int32_t x_pixels_per_meter, y_pixels_per_meter;
  uint32_t colors_used, colors_important;
  uint32_t colors[1];
};
struct WindowClass {
  uint32_t size, style;
  void* procedure;
  int32_t class_extra, window_extra;
  Handle instance, icon, cursor, background;
  const char16_t* menu_name;
  const char16_t* class_name;
  Handle small_icon;
};
static_assert(sizeof(BitmapInfo) == 44, "BITMAPINFO with one color");
static_assert(sizeof(WindowClass) == 80, "WNDCLASSEXW");

constexpr uint32_t kStyleExLayered = 0x00080000;
constexpr uint32_t kStyleExTransparent = 0x00000020;
constexpr uint32_t kStyleExToolWindow = 0x00000080;
constexpr uint32_t kStyleExNoActivate = 0x08000000;
constexpr uint32_t kStylePopup = 0x80000000;
constexpr int32_t kShowHide = 0;
constexpr int32_t kShowNoActivate = 4;
constexpr uint32_t kLayerAlpha = 2;  // ULW_ALPHA
constexpr uint32_t kMaximumSide = 8192;
constexpr char16_t kClassName[] = u"Blox7TextLayer";

struct Api {
  uint16_t(MS* RegisterClassExW)(const WindowClass*);
  Handle(MS* CreateWindowExW)(uint32_t, const char16_t*, const char16_t*, uint32_t, int32_t, int32_t,
                              int32_t, int32_t, Handle, Handle, Handle, void*);
  int32_t(MS* DestroyWindow)(Handle);
  int32_t(MS* ShowWindow)(Handle, int32_t);
  int32_t(MS* UpdateLayeredWindow)(Handle, Handle, Point*, Size*, Handle, Point*, uint32_t,
                                   BlendFunction*, uint32_t);
  int32_t(MS* GetClientRect)(Handle, Rect*);
  int32_t(MS* ClientToScreen)(Handle, Point*);
  int32_t(MS* IsIconic)(Handle);
  int32_t(MS* IsWindowVisible)(Handle);
  void* DefWindowProcW;
  Handle(MS* CreateCompatibleDC)(Handle);
  int32_t(MS* DeleteDC)(Handle);
  Handle(MS* CreateDIBSection)(Handle, const BitmapInfo*, uint32_t, void**, Handle, uint32_t);
  Handle(MS* SelectObject)(Handle, Handle);
  int32_t(MS* DeleteObject)(Handle);
  Handle(MS* GetModuleHandleW)(const char16_t*);

  template <typename T>
  static bool Find(void* module, const char* name, T* slot) {
    *slot = reinterpret_cast<T>(__w32_pe_symbol(module, name));
    return *slot != nullptr;
  }

  bool Load() {
    void* user32 = __w32_pe_open("user32.dll");
    void* gdi32 = __w32_pe_open("gdi32.dll");
    void* kernel32 = __w32_pe_open("kernel32.dll");
    return user32 != nullptr && gdi32 != nullptr && kernel32 != nullptr &&
           Find(user32, "RegisterClassExW", &RegisterClassExW) &&
           Find(user32, "CreateWindowExW", &CreateWindowExW) &&
           Find(user32, "DestroyWindow", &DestroyWindow) &&
           Find(user32, "ShowWindow", &ShowWindow) &&
           Find(user32, "UpdateLayeredWindow", &UpdateLayeredWindow) &&
           Find(user32, "GetClientRect", &GetClientRect) &&
           Find(user32, "ClientToScreen", &ClientToScreen) &&
           Find(user32, "IsIconic", &IsIconic) &&
           Find(user32, "IsWindowVisible", &IsWindowVisible) &&
           Find(user32, "DefWindowProcW", &DefWindowProcW) &&
           Find(gdi32, "CreateCompatibleDC", &CreateCompatibleDC) &&
           Find(gdi32, "DeleteDC", &DeleteDC) &&
           Find(gdi32, "CreateDIBSection", &CreateDIBSection) &&
           Find(gdi32, "SelectObject", &SelectObject) &&
           Find(gdi32, "DeleteObject", &DeleteObject) &&
           Find(kernel32, "GetModuleHandleW", &GetModuleHandleW);
  }
};

struct Layer {
  Api api{};
  bool loaded = false;
  bool failed = false;
  bool registered = false;
  Handle window = nullptr;
  Handle owner = nullptr;
  Handle dc = nullptr;
  Handle bitmap = nullptr;
  Handle previous_bitmap = nullptr;
  uint8_t* bits = nullptr;
  int32_t width = 0;
  int32_t height = 0;
  bool shown = false;
  uint64_t revision = 0;
  Point position{};
  std::vector<uint8_t> rgba;
};

Layer g_layer;

void Fail(const char* what) {
  if (!g_layer.failed) {
    std::fprintf(stderr, "  [text-layer] %s: the focused TextBox cannot be shown\n", what);
  }
  g_layer.failed = true;
}

// typed text (passwords too) lives in this image, wipe it
void Wipe(void* bytes, std::size_t size) {
  volatile uint8_t* p = static_cast<volatile uint8_t*>(bytes);
  for (std::size_t i = 0; i < size; ++i) p[i] = 0;
}

void Hide() {
  Layer& layer = g_layer;
  if (layer.window != nullptr && layer.shown) {
    layer.api.ShowWindow(layer.window, kShowHide);
  }
  layer.shown = false;
  layer.revision = 0;
  if (layer.bits != nullptr) {
    Wipe(layer.bits, static_cast<std::size_t>(layer.width) * layer.height * 4);
  }
}

void ReleaseBitmap() {
  Layer& layer = g_layer;
  if (layer.bitmap != nullptr) {
    layer.api.SelectObject(layer.dc, layer.previous_bitmap);
    layer.api.DeleteObject(layer.bitmap);
  }
  layer.bitmap = nullptr;
  layer.previous_bitmap = nullptr;
  layer.bits = nullptr;
  layer.width = 0;
  layer.height = 0;
}

bool EnsureWindow(Handle owner) {
  Layer& layer = g_layer;
  if (layer.window != nullptr && layer.owner == owner) {
    return true;
  }
  if (layer.window != nullptr) {
    layer.api.DestroyWindow(layer.window);
    layer.window = nullptr;
    layer.shown = false;
  }
  Handle instance = layer.api.GetModuleHandleW(nullptr);
  if (!layer.registered) {
    WindowClass window_class{};
    window_class.size = sizeof window_class;
    window_class.procedure = layer.api.DefWindowProcW;
    window_class.instance = instance;
    window_class.class_name = kClassName;
    layer.registered = layer.api.RegisterClassExW(&window_class) != 0;
    if (!layer.registered) {
      Fail("RegisterClassExW failed");
      return false;
    }
  }
  layer.window = layer.api.CreateWindowExW(
      kStyleExLayered | kStyleExTransparent | kStyleExToolWindow | kStyleExNoActivate, kClassName,
      u"", kStylePopup, 0, 0, 1, 1, owner, nullptr, instance, nullptr);
  if (layer.window == nullptr) {
    Fail("CreateWindowExW failed");
    return false;
  }
  layer.owner = owner;
  return true;
}

bool EnsureBitmap(int32_t width, int32_t height) {
  Layer& layer = g_layer;
  if (layer.bitmap != nullptr && layer.width == width && layer.height == height) {
    return true;
  }
  ReleaseBitmap();
  if (layer.dc == nullptr) {
    layer.dc = layer.api.CreateCompatibleDC(nullptr);
    if (layer.dc == nullptr) {
      Fail("CreateCompatibleDC failed");
      return false;
    }
  }
  BitmapInfo info{};
  info.size = 40;
  info.width = width;
  info.height = -height;
  info.planes = 1;
  info.bit_count = 32;
  void* pixels = nullptr;
  layer.bitmap = layer.api.CreateDIBSection(layer.dc, &info, 0, &pixels, nullptr, 0);
  if (layer.bitmap == nullptr || pixels == nullptr) {
    layer.bitmap = nullptr;
    Fail("CreateDIBSection failed");
    return false;
  }
  layer.previous_bitmap = layer.api.SelectObject(layer.dc, layer.bitmap);
  layer.bits = static_cast<uint8_t*>(pixels);
  layer.width = width;
  layer.height = height;
  return true;
}

bool ValidFrame(const Blox7TextOverlayFrameInfo& frame) {
  const uint64_t row_bytes = uint64_t{frame.pixel_width} * 4;
  return frame.abi_version == Blox7TextOverlayFrameInfo::kAbiVersion &&
         frame.coordinate_width != 0 && frame.coordinate_height != 0 && frame.pixel_width != 0 &&
         frame.pixel_height != 0 && frame.pixel_width <= kMaximumSide &&
         frame.pixel_height <= kMaximumSide && frame.row_bytes == row_bytes &&
         frame.rgba_bytes == row_bytes * frame.pixel_height;
}

}  // namespace

bool UpdateTextLayer(void* game_window) {
  Layer& layer = g_layer;
  if (layer.failed) {
    return false;
  }
  Blox7TextOverlayFrameInfo frame{};
  if (game_window == nullptr || !blox7_text_overlay_may_present() ||
      !blox7_text_overlay_query(&frame) || frame.visible == 0 || !ValidFrame(frame)) {
    Hide();
    return false;
  }
  if (!layer.loaded) {
    if (!layer.api.Load()) {
      Fail("user32 or gdi32 functions are missing");
      return false;
    }
    layer.loaded = true;
  }
  if (layer.api.IsIconic(game_window) || !layer.api.IsWindowVisible(game_window)) {
    Hide();
    return false;
  }
  if (!EnsureWindow(game_window)) {
    return false;
  }
  Rect client{};
  if (!layer.api.GetClientRect(game_window, &client) || client.right <= 0 || client.bottom <= 0) {
    Hide();
    return false;
  }
  Point position{
      static_cast<int32_t>(std::lround(double(frame.x) * client.right / frame.coordinate_width)),
      static_cast<int32_t>(std::lround(double(frame.y) * client.bottom / frame.coordinate_height))};
  if (!layer.api.ClientToScreen(game_window, &position)) {
    Hide();
    return false;
  }
  Size size{static_cast<int32_t>(frame.pixel_width), static_cast<int32_t>(frame.pixel_height)};
  const bool new_image =
      frame.revision != layer.revision || size.cx != layer.width || size.cy != layer.height;
  const bool moved = position.x != layer.position.x || position.y != layer.position.y;
  if (layer.shown && !new_image && !moved) {
    return false;
  }
  if (new_image) {
    layer.rgba.resize(static_cast<std::size_t>(frame.rgba_bytes));
    if (!blox7_text_overlay_copy(frame.revision, layer.rgba.data(), layer.rgba.size())) {
      Wipe(layer.rgba.data(), layer.rgba.size());
      return false;
    }
    if (!EnsureBitmap(size.cx, size.cy)) {
      Wipe(layer.rgba.data(), layer.rgba.size());
      return false;
    }
    const uint8_t* source = layer.rgba.data();
    uint8_t* target = layer.bits;
    const std::size_t pixels = static_cast<std::size_t>(size.cx) * size.cy;
    for (std::size_t i = 0; i < pixels; ++i, source += 4, target += 4) {
      const uint32_t alpha = source[3];
      target[0] = static_cast<uint8_t>((source[2] * alpha + 127) / 255);
      target[1] = static_cast<uint8_t>((source[1] * alpha + 127) / 255);
      target[2] = static_cast<uint8_t>((source[0] * alpha + 127) / 255);
      target[3] = static_cast<uint8_t>(alpha);
    }
    Wipe(layer.rgba.data(), layer.rgba.size());
  }
  Point origin{0, 0};
  BlendFunction blend{0, 0, 255, 1};
  if (!layer.api.UpdateLayeredWindow(layer.window, nullptr, &position, &size, layer.dc, &origin, 0,
                                     &blend, kLayerAlpha)) {
    Fail("UpdateLayeredWindow failed");
    Hide();
    return false;
  }
  layer.position = position;
  layer.revision = frame.revision;
  if (!layer.shown) {
    layer.api.ShowWindow(layer.window, kShowNoActivate);
    layer.shown = true;
  }
  return new_image;
}

void DestroyTextLayer() {
  Layer& layer = g_layer;
  if (!layer.loaded) {
    return;
  }
  Hide();
  ReleaseBitmap();
  if (layer.dc != nullptr) {
    layer.api.DeleteDC(layer.dc);
    layer.dc = nullptr;
  }
  if (layer.window != nullptr) {
    layer.api.DestroyWindow(layer.window);
    layer.window = nullptr;
  }
  layer.owner = nullptr;
}

}  // namespace blox7::win32

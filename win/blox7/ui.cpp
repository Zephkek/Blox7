#include <d2d1.h>
#include <dwrite.h>
#include <commdlg.h>
#include <shellapi.h>
#include <wincodec.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cwchar>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <map>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "blox7.h"

namespace blox7 {
namespace {

constexpr wchar_t kMainClass[] = L"Blox7.Launcher";
constexpr wchar_t kDialogClass[] = L"Blox7.Settings";
constexpr UINT kUnpackProgress = WM_APP + 1;
constexpr UINT kUnpackDone = WM_APP + 2;
constexpr UINT_PTR kAnimTimer = 1;
constexpr UINT_PTR kWatchTimer = 2;
constexpr UINT_PTR kCaretTimer = 3;
constexpr UINT kAboutCommand = 0x0010;

constexpr float kMargin = 11, kMainWidth = 460, kCommandHeight = 45;
constexpr float kDialogWidth = 424, kDialogHeight = 360, kTabHeight = 21;
constexpr float kButtonWidth = 75, kButtonHeight = 23, kGap = 7;

constexpr uint32_t kWhite = 0xFFFFFF, kBlack = 0x000000, kFace = 0xF0F0F0, kSeparator = 0xDFDFDF;
constexpr uint32_t kInstruction = 0x003399, kText = 0x000000, kSecondary = 0x5D5D5D, kDisabled = 0x838383;
constexpr uint32_t kLink = 0x0066CC, kLinkHot = 0x3399FF, kHighlight = 0x3399FF, kError = 0xC42B1C;
constexpr uint32_t kGroupEdge = 0xD5DFE5, kTabEdge = 0x898C95, kListEdge = 0x828790, kHeaderText = 0x4C607A;

template <class T>
void release(T*& p) {
  if (p) p->Release();
  p = nullptr;
}

D2D1_COLOR_F rgb(uint32_t hex, float alpha = 1.0f) { return D2D1::ColorF(hex, alpha); }
D2D1_RECT_F rect(float x, float y, float w, float h) { return D2D1::RectF(x, y, x + w, y + h); }
D2D1_RECT_F inset(const D2D1_RECT_F& r, float by) { return D2D1::RectF(r.left + by, r.top + by, r.right - by, r.bottom - by); }
D2D1_POINT_2F pt(float x, float y) { return D2D1::Point2F(x, y); }
bool inside(const D2D1_RECT_F& r, float x, float y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }
float smooth(float t) { return t * t * (3 - 2 * t); }

uint32_t mix(uint32_t a, uint32_t b, float t) {
  uint32_t out = 0;
  for (int shift = 0; shift <= 16; shift += 8) {
    const float x = static_cast<float>((a >> shift) & 0xFF), y = static_cast<float>((b >> shift) & 0xFF);
    out |= static_cast<uint32_t>(std::lround(x + (y - x) * t)) << shift;
  }
  return out;
}

enum class Stage { Unpacking, Ready, Launching, Running, Failed };
enum class Tab { Graphics, Display, Effects, Performance, Flags, Device, About };
enum class Look { Normal, Hot, Pressed, Disabled };
enum class Glyph { Phone, Tablet, Pc, Console };
enum class Cursor { Arrow, Hand, Beam };

enum : int {
  kPlayButton = 1,
  kSettingsButton,
  kDetailsButton,
  kOkButton,
  kCancelButton,
  kApplyButton,
  kAddFlag,
  kRemoveFlag,
  kNameField,
  kValueField,
  kFindField,
  kImportFlags,
  kExportFlags,
  kDataLink,
  kLogsLink,
  kLicensesLink,
  kBrowseShader,
  kRemoveShader,
  kTabFirst = 100,
  kFlagRowFirst = 200,
  kBackdrop = 899,
  kPopupFirst = 900,
  kOptionFirst = 1000,
  kStateFirst = 1 << 20,
};

enum Control : int {
  kRenderer,
  kQuality,
  kRenderScale,
  kGpu,
  kWindowSize,
  kFullscreen,
  kHighDpi,
  kRefresh,
  kFrameRate,
  kVsync,
  kWorkers,
  kThreadedRendering,
  kProfile,
  kEffects,
  kSharpen,
  kVibrance,
  kContrast,
  kBrightness,
  kFxaa,
  kJoinLinks,
};

constexpr int option_id(Control control, size_t option) { return kOptionFirst + control * 100 + static_cast<int>(option); }

constexpr struct {
  Tab tab;
  const wchar_t* label;
} kTabs[] = {{Tab::Graphics, L"Graphics"}, {Tab::Display, L"Display"},       {Tab::Effects, L"Effects"},
             {Tab::Performance, L"Performance"}, {Tab::Flags, L"Flags"}, {Tab::Device, L"Device"},
             {Tab::About, L"About"}};

struct Spring {
  float value = 0, target = 0;
};

struct Hit {
  D2D1_RECT_F area;
  int id;
  std::function<void()> action;
  Cursor cursor;
  std::function<void(float)> drag;
};

struct Stop {
  float at;
  uint32_t color;
  float alpha = 1;
};

struct Popup {
  bool active = false;
  D2D1_RECT_F anchor{};
  std::vector<std::wstring> labels;
  size_t selected = 0;
  std::function<void(size_t)> pick;
};

struct Fonts {
  IDWriteTextFormat *body = nullptr, *centered = nullptr, *field = nullptr, *instruction = nullptr, *paragraph = nullptr,
                    *mono = nullptr, *strong = nullptr;
};

struct Canvas {
  HWND hwnd = nullptr;
  float dpi = 96, width = 0, height = 0;
  ID2D1RenderTarget* rt = nullptr;
  ID2D1HwndRenderTarget* window_rt = nullptr;
  ID2D1SolidColorBrush* brush = nullptr;
  std::map<uint64_t, ID2D1LinearGradientBrush*> gradients;
  std::vector<Hit> hits;
  std::map<int, Spring> springs;
  Popup popup;
  int open_combo = -1, hot = 0, pressed = 0;
  float mouse_x = -1, mouse_y = -1;
  bool tracking = false, timer_on = false, active = false;
  ULONGLONG last_tick = 0;
  void (*draw)() = nullptr;
  std::function<void(float)> drag;
};

struct App {
  Paths paths;
  OsInfo os;
  Settings settings, pending;
  std::vector<FastFlag> flags;
  std::vector<GpuChoice> gpus;
  std::vector<DeviceChoice> devices;
  std::vector<int> rates;
  int monitor_hz = 60, screen_w = 1920, screen_h = 1080;

  ID2D1Factory* d2d = nullptr;
  IDWriteFactory* dw = nullptr;
  ID2D1StrokeStyle* round = nullptr;
  ID2D1PathGeometry* logo[4] = {};
  Fonts fonts;
  Canvas main, dialog;

  Stage stage = Stage::Ready;
  std::atomic<double> unpacked{0};
  std::wstring version, backend, detail;
  bool vulkan_driver = true;
  std::vector<std::wstring> error_lines;
  DWORD exit_code = 0;
  bool details = false;
  float clock = 0;

  Tab tab = Tab::Graphics;
  bool dirty = false;
  std::wstring name_text, value_text, find_text, flag_message;
  bool flag_error = false;
  int focus = 0;
  size_t caret = 0;
  bool caret_on = true;
  int flag_selected = -1;
  int flag_scroll = 0;
  D2D1_RECT_F flag_list_area{};
  std::wstring shader_status;
  bool shader_error = false;

  PROCESS_INFORMATION child{};
  bool window_seen = false;
  bool minimized_by_us = false;
  FILETIME launched{};
};

App g;
Canvas* c = &g.main;
std::unordered_map<std::wstring, std::wstring> g_fitted;

void launch();
void open_settings(Tab tab);

IDWriteTextFormat* make_font(float size, DWRITE_FONT_WEIGHT weight, DWRITE_TEXT_ALIGNMENT align, bool middle, bool single_line,
                             const wchar_t* family = L"Segoe UI") {
  IDWriteTextFormat* format = nullptr;
  if (FAILED(g.dw->CreateTextFormat(family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size,
                                    L"en-us", &format)))
    return nullptr;
  format->SetTextAlignment(align);
  if (middle) format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
  if (single_line) {
    format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    IDWriteInlineObject* dots = nullptr;
    if (SUCCEEDED(g.dw->CreateEllipsisTrimmingSign(format, &dots))) {
      DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
      format->SetTrimming(&trimming, dots);
      dots->Release();
    }
  }
  return format;
}

void create_fonts() {
  g_fitted.clear();
  constexpr auto lead = DWRITE_TEXT_ALIGNMENT_LEADING;
  constexpr auto normal = DWRITE_FONT_WEIGHT_NORMAL;
  Fonts& f = g.fonts;
  f.body = make_font(12, normal, lead, false, true);
  f.strong = make_font(12, DWRITE_FONT_WEIGHT_BOLD, lead, false, true);
  f.centered = make_font(12, normal, DWRITE_TEXT_ALIGNMENT_CENTER, true, true);
  f.field = make_font(12, normal, lead, true, true);
  f.instruction = make_font(16, normal, lead, false, true);
  f.paragraph = make_font(12, normal, lead, false, false);
  if (f.paragraph) f.paragraph->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, 15, 12);
  f.mono = make_font(12, normal, lead, false, true, L"Consolas");
}

void drop_device(Canvas& canvas) {
  for (auto& entry : canvas.gradients) release(entry.second);
  canvas.gradients.clear();
  release(canvas.brush);
  canvas.rt = nullptr;
  release(canvas.window_rt);
}

bool create_device(Canvas& canvas) {
  if (canvas.rt) return true;
  RECT client;
  GetClientRect(canvas.hwnd, &client);
  const D2D1_SIZE_U size = D2D1::SizeU(static_cast<UINT32>(client.right), static_cast<UINT32>(client.bottom));
  const D2D1_RENDER_TARGET_PROPERTIES props =
      D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
                                   canvas.dpi, canvas.dpi);
  if (FAILED(g.d2d->CreateHwndRenderTarget(props, D2D1::HwndRenderTargetProperties(canvas.hwnd, size), &canvas.window_rt)))
    return false;
  canvas.rt = canvas.window_rt;
  canvas.rt->CreateSolidColorBrush(rgb(kText), &canvas.brush);
  return canvas.brush != nullptr;
}

ID2D1Brush* ink(uint32_t color, float alpha = 1) {
  c->brush->SetColor(rgb(color, alpha));
  return c->brush;
}

// d2d charges per gradient so we cache them
ID2D1LinearGradientBrush* gradient(std::initializer_list<Stop> stops, D2D1_POINT_2F from, D2D1_POINT_2F to) {
  uint64_t key = 1469598103934665603ULL;
  const auto feed = [&key](uint64_t value) { key = (key ^ value) * 1099511628211ULL; };
  D2D1_GRADIENT_STOP raw[8];
  UINT32 count = 0;
  for (const Stop& stop : stops) {
    feed(static_cast<uint64_t>(stop.at * 10000));
    feed(stop.color);
    feed(static_cast<uint64_t>(stop.alpha * 1000));
    if (count < 8) raw[count++] = {stop.at, rgb(stop.color, stop.alpha)};
  }
  const auto found = c->gradients.find(key);
  ID2D1LinearGradientBrush* brush = found == c->gradients.end() ? nullptr : found->second;
  if (!brush) {
    ID2D1GradientStopCollection* collection = nullptr;
    if (SUCCEEDED(c->rt->CreateGradientStopCollection(raw, count, &collection))) {
      c->rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(from, to), collection, &brush);
      collection->Release();
    }
    if (!brush) return nullptr;
    c->gradients[key] = brush;
  }
  brush->SetStartPoint(from);
  brush->SetEndPoint(to);
  brush->SetOpacity(1);
  return brush;
}

void fill_rect(const D2D1_RECT_F& r, ID2D1Brush* brush) {
  if (brush) c->rt->FillRectangle(&r, brush);
}

void fill_round(const D2D1_RECT_F& r, float radius, ID2D1Brush* brush) {
  if (!brush) return;
  if (radius <= 0) {
    c->rt->FillRectangle(&r, brush);
    return;
  }
  const D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(r, radius, radius);
  c->rt->FillRoundedRectangle(&rr, brush);
}

void stroke_round(const D2D1_RECT_F& r, float radius, uint32_t color, float alpha = 1) {
  const D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(r, radius, radius);
  c->rt->DrawRoundedRectangle(&rr, ink(color, alpha), 1);
}

void stroke_rect(const D2D1_RECT_F& r, uint32_t color, float alpha = 1) {
  const D2D1_RECT_F edge = inset(r, 0.5f);
  c->rt->DrawRectangle(&edge, ink(color, alpha), 1);
}

void fill_vgrad(const D2D1_RECT_F& r, float radius, std::initializer_list<Stop> stops, float opacity = 1) {
  ID2D1LinearGradientBrush* brush = gradient(stops, pt(r.left, r.top), pt(r.left, r.bottom));
  if (!brush) return;
  brush->SetOpacity(opacity);
  fill_round(r, radius, brush);
}

void hline(float x1, float x2, float y, uint32_t color, float alpha = 1) {
  c->rt->DrawLine(pt(x1, y + 0.5f), pt(x2, y + 0.5f), ink(color, alpha), 1);
}

void vline(float x, float y1, float y2, uint32_t color, float alpha = 1) {
  c->rt->DrawLine(pt(x + 0.5f, y1), pt(x + 0.5f, y2), ink(color, alpha), 1);
}

void dot(D2D1_POINT_2F center, float rx, float ry, ID2D1Brush* brush) {
  if (!brush) return;
  const D2D1_ELLIPSE e = D2D1::Ellipse(center, rx, ry);
  c->rt->FillEllipse(&e, brush);
}

void ring(D2D1_POINT_2F center, float radius, ID2D1Brush* brush) {
  const D2D1_ELLIPSE e = D2D1::Ellipse(center, radius, radius);
  c->rt->DrawEllipse(&e, brush, 1);
}

void path(std::initializer_list<D2D1_POINT_2F> points, ID2D1Brush* brush, float width, bool closed, bool filled = false) {
  ID2D1PathGeometry* geometry = nullptr;
  ID2D1GeometrySink* sink = nullptr;
  if (!brush || FAILED(g.d2d->CreatePathGeometry(&geometry)) || FAILED(geometry->Open(&sink))) {
    release(geometry);
    return;
  }
  const D2D1_POINT_2F* p = points.begin();
  sink->BeginFigure(*p, filled ? D2D1_FIGURE_BEGIN_FILLED : D2D1_FIGURE_BEGIN_HOLLOW);
  for (++p; p != points.end(); ++p) sink->AddLine(*p);
  sink->EndFigure(closed ? D2D1_FIGURE_END_CLOSED : D2D1_FIGURE_END_OPEN);
  sink->Close();
  sink->Release();
  if (filled) c->rt->FillGeometry(geometry, brush);
  else c->rt->DrawGeometry(geometry, brush, width, g.round);
  geometry->Release();
}

void heart(float cx, float cy) {
  ID2D1Brush* red = ink(0xD13438);
  dot(pt(cx - 2.2f, cy - 1.6f), 2.6f, 2.6f, red);
  dot(pt(cx + 2.2f, cy - 1.6f), 2.6f, 2.6f, red);
  path({pt(cx - 4.7f, cy - 0.9f), pt(cx + 4.7f, cy - 0.9f), pt(cx, cy + 4.2f)}, red, 1, true, true);
}

void warning_sign(float x, float y) {
  path({pt(x + 7, y), pt(x + 14, y + 13), pt(x, y + 13)}, gradient({{0, 0xFFE57A}, {1, 0xF4B400}}, pt(0, y), pt(0, y + 13)), 1,
       true, true);
  path({pt(x + 7, y + 0.5f), pt(x + 13.5f, y + 12.5f), pt(x + 0.5f, y + 12.5f)}, ink(0xB78B00), 1, true);
  path({pt(x + 7, y + 4), pt(x + 7, y + 8.5f)}, ink(0x000000), 1.6f, false);
  dot(pt(x + 7, y + 10.6f), 0.9f, 0.9f, ink(0x000000));
}

float text_width(const std::wstring& s, IDWriteTextFormat* format) {
  IDWriteTextLayout* layout = nullptr;
  DWRITE_TEXT_METRICS metrics{};
  if (format && SUCCEEDED(g.dw->CreateTextLayout(s.c_str(), static_cast<UINT32>(s.size()), format, 4000, 200, &layout))) {
    layout->GetMetrics(&metrics);
    layout->Release();
  }
  return metrics.widthIncludingTrailingWhitespace;
}

// directwrite's trimming ellipsis isn't ascii, three dots it is

const std::wstring& fitted(const std::wstring& s, IDWriteTextFormat* format, float room) {
  std::wstring key = s + L'\x1f' + std::to_wstring(reinterpret_cast<uintptr_t>(format)) + L'\x1f' +
                     std::to_wstring(static_cast<int>(room * 4));
  if (const auto found = g_fitted.find(key); found != g_fitted.end()) return found->second;
  std::wstring shown = s;
  if (text_width(s, format) > room + 0.5f) {
    size_t lo = 0, hi = s.size();
    while (lo < hi) {
      const size_t mid = (lo + hi + 1) / 2;
      if (text_width(s.substr(0, mid) + L"...", format) <= room) lo = mid;
      else hi = mid - 1;
    }
    shown = s.substr(0, lo);
    while (!shown.empty() && shown.back() == L' ') shown.pop_back();
    shown += L"...";
  }
  if (g_fitted.size() > 4096) g_fitted.clear();
  return g_fitted.emplace(std::move(key), std::move(shown)).first->second;
}

void text(const std::wstring& s, IDWriteTextFormat* format, const D2D1_RECT_F& area, uint32_t color, float alpha = 1) {
  if (!format) return;
  const std::wstring& shown =
      format->GetWordWrapping() == DWRITE_WORD_WRAPPING_NO_WRAP ? fitted(s, format, area.right - area.left) : s;
  c->rt->DrawText(shown.c_str(), static_cast<UINT32>(shown.size()), format, &area, ink(color, alpha),
                  D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

ID2D1PathGeometry* combine(ID2D1Geometry* a, ID2D1Geometry* b, D2D1_COMBINE_MODE mode) {
  ID2D1PathGeometry* out = nullptr;
  ID2D1GeometrySink* sink = nullptr;
  if (!a || !b || FAILED(g.d2d->CreatePathGeometry(&out)) || FAILED(out->Open(&sink))) {
    release(out);
    return nullptr;
  }
  const HRESULT hr = a->CombineWithGeometry(b, mode, nullptr, 0.02f, sink);
  sink->Close();
  sink->Release();
  if (FAILED(hr)) release(out);
  return out;
}

void build_logo() {
  constexpr float half = 50, gap = 3.4f, counter = 14, corner = 7;
  ID2D1RoundedRectangleGeometry* outer = nullptr;
  ID2D1RectangleGeometry* hole = nullptr;
  g.d2d->CreateRoundedRectangleGeometry(D2D1::RoundedRect(D2D1::RectF(-half, -half, half, half), corner, corner), &outer);
  g.d2d->CreateRectangleGeometry(D2D1::RectF(-counter, -counter, counter, counter), &hole);
  const D2D1_RECT_F panes[4] = {D2D1::RectF(-half, -half, -gap / 2, -gap / 2), D2D1::RectF(gap / 2, -half, half, -gap / 2),
                                D2D1::RectF(-half, gap / 2, -gap / 2, half), D2D1::RectF(gap / 2, gap / 2, half, half)};
  for (int i = 0; i < 4; ++i) {
    ID2D1RectangleGeometry* pane = nullptr;
    g.d2d->CreateRectangleGeometry(panes[i], &pane);
    ID2D1PathGeometry* clipped = combine(outer, pane, D2D1_COMBINE_MODE_INTERSECT);
    g.logo[i] = combine(clipped, hole, D2D1_COMBINE_MODE_EXCLUDE);
    release(clipped);
    release(pane);
  }
  release(outer);
  release(hole);
}

// d2d1 is loaded by hand so no Rotation() for us. trig time
D2D1_MATRIX_3X2_F tilt() {
  const float a = 15 * 3.14159265f / 180;
  return D2D1::Matrix3x2F(std::cos(a), std::sin(a), -std::sin(a), std::cos(a), 0, 0);
}

void draw_logo(float cx, float cy, float size) {
  static const uint32_t light[4] = {0xF7865C, 0xB4DF5E, 0x5ABFF3, 0xFFDB68};
  static const uint32_t deep[4] = {0xD8391A, 0x5BA526, 0x0E6CC3, 0xEE9D0E};
  static const float tops[4] = {-50, -50, 1.7f, 1.7f};
  static const float bottoms[4] = {-1.7f, -1.7f, 50, 50};
  D2D1_MATRIX_3X2_F saved;
  c->rt->GetTransform(&saved);
  const float k = size / 100;
  const auto place = [&](float dy) {
    c->rt->SetTransform(D2D1::Matrix3x2F::Scale(k, k) * tilt() * D2D1::Matrix3x2F::Translation(cx, cy + dy) * saved);
  };
  place(size * 0.04f);
  for (ID2D1PathGeometry* part : g.logo)
    if (part) c->rt->FillGeometry(part, ink(0x0A1E3C, 0.14f));
  place(0);
  for (int i = 0; i < 4; ++i) {
    if (!g.logo[i]) continue;
    if (ID2D1Brush* body = gradient({{0, light[i]}, {1, deep[i]}}, pt(0, tops[i]), pt(0, bottoms[i])))
      c->rt->FillGeometry(g.logo[i], body);
    if (ID2D1Brush* gloss =
            gradient({{0, kWhite, 0.36f}, {0.5f, kWhite, 0.08f}, {0.5f, kWhite, 0}, {1, kWhite, 0}}, pt(0, -50), pt(0, 50)))
      c->rt->FillGeometry(g.logo[i], gloss);
    c->rt->DrawGeometry(g.logo[i], ink(deep[i], 0.6f), std::max(1.0f, 1.1f / k));
  }
  c->rt->SetTransform(saved);
}

void draw_glyph(Glyph glyph, float x, float y, float size) {
  D2D1_MATRIX_3X2_F saved;
  c->rt->GetTransform(&saved);
  const float k = size / 16;
  c->rt->SetTransform(D2D1::Matrix3x2F::Scale(k, k) * D2D1::Matrix3x2F::Translation(x, y) * saved);
  switch (glyph) {
    case Glyph::Phone:
      fill_vgrad(D2D1::RectF(3.5f, 0.5f, 12.5f, 15.5f), 2, {{0, 0x4B5A6E}, {1, 0x222C38}});
      fill_vgrad(D2D1::RectF(4.8f, 2.3f, 11.2f, 12.6f), 0.5f, {{0, 0x82CFFB}, {1, 0x2B79C8}});
      dot(pt(8, 14), 0.8f, 0.8f, ink(0x9AA7B6));
      break;
    case Glyph::Tablet:
      fill_vgrad(D2D1::RectF(1.5f, 1, 14.5f, 15), 1.8f, {{0, 0x4B5A6E}, {1, 0x222C38}});
      fill_vgrad(D2D1::RectF(2.8f, 2.5f, 13.2f, 12.6f), 0.5f, {{0, 0x82CFFB}, {1, 0x2B79C8}});
      dot(pt(8, 13.8f), 0.7f, 0.7f, ink(0x9AA7B6));
      break;
    case Glyph::Pc:
      fill_vgrad(D2D1::RectF(0.5f, 1.5f, 15.5f, 11.5f), 1.5f, {{0, 0x5A6B80}, {1, 0x2C3846}});
      fill_vgrad(D2D1::RectF(2, 3, 14, 10), 0.5f, {{0, 0x82CFFB}, {1, 0x2B79C8}});
      path({pt(2, 3), pt(14, 3), pt(2, 8)}, ink(kWhite, 0.22f), 0, true, true);
      fill_rect(D2D1::RectF(6.6f, 11.5f, 9.4f, 13.4f), ink(0x6F7E92));
      fill_vgrad(D2D1::RectF(4, 13.4f, 12, 15), 0.7f, {{0, 0x9AA6B5}, {1, 0x5D6A7C}});
      break;
    case Glyph::Console:
      fill_vgrad(D2D1::RectF(0.5f, 4, 15.5f, 12.5f), 4, {{0, 0x5E6C7E}, {1, 0x29333F}});
      fill_rect(D2D1::RectF(3.2f, 7.6f, 6.6f, 8.9f), ink(kWhite, 0.9f));
      fill_rect(D2D1::RectF(4.25f, 6.55f, 5.55f, 9.95f), ink(kWhite, 0.9f));
      dot(pt(10.7f, 7.4f), 0.95f, 0.95f, ink(0xF25022));
      dot(pt(12.3f, 8.8f), 0.95f, 0.95f, ink(0x7FBA00));
      dot(pt(9.9f, 9.3f), 0.95f, 0.95f, ink(0x00A4EF));
      dot(pt(11.5f, 10.7f), 0.95f, 0.95f, ink(0xFFB900));
      break;
  }
  c->rt->SetTransform(saved);
}

float spring(int key, float target) {
  Spring& s = c->springs.try_emplace(key, Spring{target, target}).first->second;
  s.target = target;
  return s.value;
}

float hover(int id) { return smooth(spring(id, c->hot == id ? 1.0f : 0.0f)); }

void clickable(int id, const D2D1_RECT_F& area, std::function<void()> action, Cursor cursor = Cursor::Arrow) {
  c->hits.push_back({area, id, std::move(action), cursor, nullptr});
}

void draggable(int id, const D2D1_RECT_F& area, std::function<void(float)> drag) {
  c->hits.push_back({area, id, nullptr, Cursor::Arrow, std::move(drag)});
}

const Hit* hit_at(float x, float y) {
  for (auto it = c->hits.rbegin(); it != c->hits.rend(); ++it)
    if (inside(it->area, x, y)) return &*it;
  return nullptr;
}

bool busy() { return g.stage == Stage::Unpacking || g.stage == Stage::Launching; }
bool can_play() { return g.stage == Stage::Ready || g.stage == Stage::Failed; }

bool animating() {
  if (c == &g.main && (busy() || (can_play() && c->active))) return true;
  if (c == &g.dialog && c->active) return true;
  for (const auto& entry : c->springs)
    if (entry.second.value != entry.second.target) return true;
  return false;
}

void sync_timer() {
  if (!c->hwnd) return;
  const bool wanted = animating();
  if (wanted && !c->timer_on) {
    c->last_tick = GetTickCount64();
    c->timer_on = SetTimer(c->hwnd, kAnimTimer, 16, nullptr) != 0;
  } else if (!wanted && c->timer_on) {
    KillTimer(c->hwnd, kAnimTimer);
    c->timer_on = false;
  }
}

void step_animation() {
  const ULONGLONG now = GetTickCount64();
  const float dt = std::min(0.1f, static_cast<float>(now - c->last_tick) / 1000.0f);
  c->last_tick = now;
  const float keep = std::exp(-dt / 0.06f);
  for (auto& entry : c->springs) {
    Spring& s = entry.second;
    s.value = s.target + (s.value - s.target) * keep;
    if (std::fabs(s.value - s.target) < 0.002f + std::fabs(s.target) * 0.0004f) s.value = s.target;
  }
  InvalidateRect(c->hwnd, nullptr, FALSE);
}

void refresh_info() {
  g.version = roblox_version(g.paths);
  g.vulkan_driver = vulkan_installed();
  g.backend = graphics_backend(g.paths);
  if (g.backend == L"Vulkan" && !g.vulkan_driver) g.backend = L"OpenGL ES (no Vulkan driver)";
}

size_t index_of(const std::vector<std::wstring>& values, const std::wstring& value) {
  return static_cast<size_t>(std::find(values.begin(), values.end(), value) - values.begin());
}

bool uses_vulkan(const std::wstring& backend) {
  return backend == L"direct-vulkan" || backend == L"vulkan" || backend == L"native-vulkan";
}

bool runs_vulkan(const std::wstring& backend) { return uses_vulkan(backend) && g.vulkan_driver; }

bool uses_gles(const std::wstring& backend) {
  return backend == L"opengl" || backend == L"gles" || backend == L"system" || backend == L"auto";
}

std::wstring canonical_device(const std::wstring& device) {
  if (device == L"pc") return L"pc-windows-11";
  if (device == L"mobile") return L"mobile-pixel-7";
  if (device == L"phone" || device == L"s22" || device == L"samsung" || device == L"samsung-s22-ultra")
    return L"mobile-s22-ultra";
  return device;
}

std::wstring device_name(const std::wstring& id) {
  const std::wstring wanted = canonical_device(id);
  for (const DeviceChoice& device : g.devices)
    if (device.id == wanted) return device.name;
  return id.empty() ? L"Custom device" : id;
}

std::wstring quality_name(const std::wstring& quality) {
  if (quality == L"performance") return L"Performance";
  if (quality == L"balanced") return L"Balanced";
  if (quality == L"high") return L"High";
  if (quality == L"superhq") return L"Super HQ";
  return L"Roblox decides";
}

Look look_of(int id, bool enabled) {
  if (!enabled) return Look::Disabled;
  if (c->pressed == id && c->hot == id) return Look::Pressed;
  return Look::Normal;
}

void aero_face(const D2D1_RECT_F& r, float radius, Look look, float alpha = 1) {
  struct Face {
    uint32_t border, t1, t2, b1, b2;
  };
  static const Face faces[] = {
      {0x707070, 0xF2F2F2, 0xEBEBEB, 0xDDDDDD, 0xCFCFCF},
      {0x3C7FB1, 0xEAF6FD, 0xD9F0FC, 0xBEE6FD, 0xA7D9F5},
      {0x2C628B, 0xE5F4FC, 0xC4E5F6, 0x98D1EF, 0x68B3DB},
      {0xADB2B5, 0xF4F4F4, 0xF4F4F4, 0xF4F4F4, 0xF4F4F4},
  };
  const Face& f = faces[static_cast<int>(look)];
  fill_vgrad(r, radius, {{0, f.t1}, {0.5f, f.t2}, {0.5f, f.b1}, {1, f.b2}}, alpha);
  stroke_round(inset(r, 1.5f), std::max(0.0f, radius - 1), kWhite, (look == Look::Pressed ? 0.2f : 0.75f) * alpha);
  stroke_round(inset(r, 0.5f), radius, f.border, alpha);
}

void button_face(int id, const D2D1_RECT_F& r, bool enabled, bool is_default = false) {
  const Look look = look_of(id, enabled);
  aero_face(r, 3, look);
  if (look != Look::Normal) return;
  float lit = hover(id);
  if (is_default && c->active) lit = std::max(lit, 0.45f * (0.5f - 0.5f * std::cos(g.clock * 2.6f)));
  if (lit > 0.01f) aero_face(r, 3, Look::Hot, lit);
  if (is_default) stroke_round(inset(r, 1.5f), 2, 0x48D8FB, 0.8f);
}

void push_button(int id, const D2D1_RECT_F& r, const std::wstring& label, bool enabled, std::function<void()> action,
                 bool is_default = false) {
  button_face(id, r, enabled, is_default && enabled);
  text(label, g.fonts.centered, r, enabled ? kText : kDisabled);
  if (enabled) clickable(id, r, std::move(action));
}

void link(int id, float x, float y, const std::wstring& label, std::function<void()> action) {
  const float hv = hover(id);
  const float w = text_width(label, g.fonts.body);
  text(label, g.fonts.body, rect(x, y, w + 2, 15), mix(kLink, kLinkHot, hv));
  if (hv > 0.5f) hline(x, x + w, y + 13.5f, kLinkHot);
  clickable(id, rect(x, y, w, 15), std::move(action), Cursor::Hand);
}

void radio_mark(float x, float y, bool checked, float hv) {
  const D2D1_POINT_2F center = pt(x + 6.5f, y + 6.5f);
  dot(center, 6, 6, ink(kWhite));
  dot(center, 4.5f, 4.5f, gradient({{0, 0xCBCFD5}, {1, 0xF6F6F6}}, pt(0, y + 2), pt(0, y + 11)));
  if (hv > 0.01f) {
    ID2D1LinearGradientBrush* lit = gradient({{0, 0xB1DFFD}, {1, 0xE9F7FE}}, pt(0, y + 2), pt(0, y + 11));
    if (lit) lit->SetOpacity(hv);
    dot(center, 4.5f, 4.5f, lit);
  }
  ring(center, 6, ink(mix(0x8E8F8F, 0x3C7FB1, hv)));
  if (!checked) return;
  dot(center, 3.2f, 3.2f, gradient({{0, 0x7AA3D6}, {1, 0x1D3A6A}}, pt(0, y + 3.3f), pt(0, y + 9.7f)));
  dot(pt(x + 5.6f, y + 5.3f), 1.2f, 0.9f, ink(kWhite, 0.7f));
}

void check_mark(float x, float y, bool checked, float hv) {
  fill_rect(D2D1::RectF(x, y, x + 13, y + 13), ink(kWhite));
  const D2D1_RECT_F inner = D2D1::RectF(x + 2, y + 2, x + 11, y + 11);
  fill_vgrad(inner, 0, {{0, 0xCBCFD5}, {1, 0xF6F6F6}});
  if (hv > 0.01f) fill_vgrad(inner, 0, {{0, 0xB1DFFD}, {1, 0xE9F7FE}}, hv);
  stroke_rect(D2D1::RectF(x, y, x + 13, y + 13), mix(0x8E8F8F, 0x3C7FB1, hv));
  if (checked) path({pt(x + 3, y + 6.8f), pt(x + 5.6f, y + 9.4f), pt(x + 10.2f, y + 3.6f)}, ink(0x33415E), 1.8f, false);
}

void radio(Control control, size_t index, float x, float y, float w, bool checked, const std::wstring& label,
           std::function<void()> pick) {
  const int id = option_id(control, index);
  const float hv = checked ? 0 : hover(id);
  radio_mark(x, y + 2, checked, hv);
  text(label, g.fonts.body, rect(x + 18, y + 1, w - 18, 15), kText);
  if (!checked) clickable(id, rect(x, y, std::min(w, text_width(label, g.fonts.body) + 22), 17), std::move(pick));
}

void checkbox(Control control, float x, float y, float w, bool checked, const std::wstring& label, std::function<void()> flip,
              bool enabled = true) {
  const int id = option_id(control, 0);
  check_mark(x, y + 2, checked, enabled ? hover(id) : 0);
  text(label, g.fonts.body, rect(x + 18, y + 1, w - 18, 15), enabled ? kText : kDisabled);
  if (enabled) clickable(id, rect(x, y, std::min(w, text_width(label, g.fonts.body) + 22), 17), std::move(flip));
}

void combo(Control control, const D2D1_RECT_F& r, const std::vector<std::wstring>& labels, size_t selected, bool enabled,
           std::function<void(size_t)> pick) {
  const int id = option_id(control, 0);
  const bool open = c->open_combo == static_cast<int>(control);
  if (open) aero_face(r, 3, Look::Pressed);
  else button_face(id, r, enabled);
  text(selected < labels.size() ? labels[selected] : L"", g.fonts.field, D2D1::RectF(r.left + 6, r.top, r.right - 20, r.bottom),
       enabled ? kText : kDisabled);
  const float ax = r.right - 11, ay = (r.top + r.bottom) / 2 - 1.5f;
  path({pt(ax - 3.5f, ay), pt(ax + 3.5f, ay), pt(ax, ay + 4)}, ink(enabled ? kText : 0x9A9A9A), 0, true, true);
  if (!enabled) return;
  const int me = static_cast<int>(control);
  clickable(id, r, [me] { c->open_combo = c->open_combo == me ? -1 : me; });
  if (open) {
    c->popup.active = true;
    c->popup.anchor = r;
    c->popup.labels = labels;
    c->popup.selected = selected;
    c->popup.pick = std::move(pick);
  }
}

void draw_popup() {
  if (!c->popup.active) return;
  clickable(kBackdrop, D2D1::RectF(0, 0, c->width, c->height), [] { c->open_combo = -1; });
  constexpr float row = 19;
  const Popup& p = c->popup;
  const float h = row * static_cast<float>(p.labels.size()) + 2;
  float top = p.anchor.bottom;
  if (top + h > c->height - 2) top = p.anchor.top - h;
  const D2D1_RECT_F list = D2D1::RectF(p.anchor.left, top, p.anchor.right, top + h);
  fill_rect(D2D1::RectF(list.left + 2, list.top + 2, list.right + 2, list.bottom + 2), ink(kBlack, 0.12f));
  fill_rect(list, ink(kWhite));
  stroke_rect(list, 0x646464);
  size_t marked = p.selected;
  for (size_t i = 0; i < p.labels.size(); ++i)
    if (c->hot == kPopupFirst + static_cast<int>(i)) marked = i;
  for (size_t i = 0; i < p.labels.size(); ++i) {
    const float y = list.top + 1 + row * static_cast<float>(i);
    const D2D1_RECT_F item = D2D1::RectF(list.left + 1, y, list.right - 1, y + row);
    if (i == marked) fill_rect(item, ink(kHighlight));
    text(p.labels[i], g.fonts.field, D2D1::RectF(item.left + 4, item.top, item.right - 4, item.bottom), i == marked ? kWhite : kText);
    const std::function<void(size_t)> pick = p.pick;
    clickable(kPopupFirst + static_cast<int>(i), item, [pick, i] {
      c->open_combo = -1;
      pick(i);
    });
  }
}

void group(const D2D1_RECT_F& r, const std::wstring& caption) {
  const float w = text_width(caption, g.fonts.body);
  stroke_round(D2D1::RectF(r.left + 0.5f, r.top + 7.5f, r.right - 0.5f, r.bottom - 0.5f), 3, kGroupEdge);
  fill_rect(rect(r.left + 6, r.top, w + 4, 15), ink(kWhite));
  text(caption, g.fonts.body, rect(r.left + 8, r.top, w + 2, 15), kText);
}

void progress_bar(const D2D1_RECT_F& r, float fraction, bool marquee) {
  fill_vgrad(r, 2.5f, {{0, 0xD3D3D3}, {0.3f, 0xEAEAEA}, {1, 0xF7F7F7}});
  stroke_round(inset(r, 0.5f), 2.5f, 0xA9A9A9);
  const D2D1_RECT_F in = inset(r, 1.5f);
  const float span = in.right - in.left;
  D2D1_RECT_F fill = in;
  if (marquee) {
    const float block = span * 0.32f, t = std::fmod(g.clock, 2.2f) / 2.2f;
    const float x = in.left - block + (span + block) * t;
    fill.left = std::max(in.left, x);
    fill.right = std::min(in.right, x + block);
  } else {
    fill.right = in.left + span * std::clamp(fraction, 0.0f, 1.0f);
  }
  if (fill.right - fill.left < 1) return;
  c->rt->PushAxisAlignedClip(&fill, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
  fill_vgrad(in, 0, {{0, 0xB7F2B2}, {0.1f, 0x7FE07E}, {0.48f, 0x43CC47}, {0.52f, 0x14AE22}, {1, 0x3ED04A}});
  fill_vgrad(D2D1::RectF(in.left, in.top, in.right, (in.top + in.bottom) / 2), 0, {{0, kWhite, 0.35f}, {1, kWhite, 0.05f}});
  if (marquee) {
    const float soft = (fill.right - fill.left) * 0.35f;
    fill_rect(D2D1::RectF(fill.left, in.top, fill.left + soft, in.bottom),
              gradient({{0, 0xE6E6E6, 1}, {1, 0xE6E6E6, 0}}, pt(fill.left, 0), pt(fill.left + soft, 0)));
    fill_rect(D2D1::RectF(fill.right - soft, in.top, fill.right, in.bottom),
              gradient({{0, 0xE6E6E6, 0}, {1, 0xE6E6E6, 1}}, pt(fill.right - soft, 0), pt(fill.right, 0)));
  } else {
    const float band = 90, t = std::fmod(g.clock, 2.6f) / 2.6f;
    const float gx = fill.left - band + (fill.right - fill.left + band * 2) * t;
    fill_rect(D2D1::RectF(gx, in.top, gx + band, in.bottom),
              gradient({{0, kWhite, 0}, {0.5f, kWhite, 0.6f}, {1, kWhite, 0}}, pt(gx, 0), pt(gx + band, 0)));
  }
  c->rt->PopAxisAlignedClip();
}

void text_field(int id, const D2D1_RECT_F& r, const std::wstring& value) {
  const bool focused = g.focus == id;
  const float hv = focused ? 0 : hover(id);
  fill_rect(r, ink(kWhite));
  uint32_t top = mix(0xABADB3, 0x5794BF, hv), sides = mix(0xE2E3EA, 0xB7D5EA, hv), bottom = mix(0xE3E9EF, 0xC7E2F1, hv);
  if (focused) {
    top = 0x3D7BAD;
    sides = 0xA4C9E3;
    bottom = 0xB7D9ED;
  }
  hline(r.left, r.right, r.top, top);
  vline(r.left, r.top + 1, r.bottom - 1, sides);
  vline(r.right - 1, r.top + 1, r.bottom - 1, sides);
  hline(r.left, r.right, r.bottom - 1, bottom);
  const D2D1_RECT_F in = D2D1::RectF(r.left + 4, r.top + 1, r.right - 4, r.bottom - 1);
  const float caret_x = focused ? text_width(value.substr(0, std::min(g.caret, value.size())), g.fonts.field) : 0;
  const float shift = std::max(0.0f, caret_x - (in.right - in.left - 2));
  c->rt->PushAxisAlignedClip(&in, D2D1_ANTIALIAS_MODE_ALIASED);
  const D2D1_RECT_F line = D2D1::RectF(in.left - shift, in.top, in.left - shift + 2000, in.bottom);
  if (c->rt && g.fonts.field) {
    IDWriteTextLayout* layout = nullptr;
    if (SUCCEEDED(g.dw->CreateTextLayout(value.c_str(), static_cast<UINT32>(value.size()), g.fonts.field, 2000,
                                         in.bottom - in.top, &layout))) {
      layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
      c->rt->DrawTextLayout(pt(line.left, line.top), layout, ink(kText));
      layout->Release();
    }
  }
  if (focused && g.caret_on) vline(in.left - shift + caret_x, in.top + 3, in.bottom - 3, kBlack);
  c->rt->PopAxisAlignedClip();
  clickable(id, r, [id, &value] {
    g.focus = id;
    g.caret = value.size();
    g.caret_on = true;
  }, Cursor::Beam);
}

void explorer_highlight(const D2D1_RECT_F& r, bool selected, float hv) {
  if (selected) {
    fill_vgrad(r, 2, {{0, 0xDCEBFC}, {1, 0xC1DBFC}});
    stroke_round(inset(r, 1.5f), 1.5f, kWhite, 0.5f);
    stroke_round(inset(r, 0.5f), 2, 0x7DA2CE);
  } else if (hv > 0.01f) {
    fill_vgrad(r, 2, {{0, 0xFAFBFD}, {1, 0xEBF3FD}}, hv);
    stroke_round(inset(r, 0.5f), 2, 0xB8D6FB, hv);
  }
}

void close_settings();
void apply_settings();

std::wstring headline() {
  switch (g.stage) {
    case Stage::Unpacking: return L"Setting up Blox7";
    case Stage::Launching: return L"Starting Roblox";
    case Stage::Running: return L"Roblox is running";
    case Stage::Failed: return L"Roblox didn't start";
    default: return g.version.empty() ? L"Ready to download Roblox" : L"Roblox " + g.version + L" is ready to play";
  }
}

std::wstring status_line() {
  wchar_t line[160];
  switch (g.stage) {
    case Stage::Unpacking:
      std::swprintf(line, 160, L"Unpacking the runtime, %d%% done. This only happens once.", static_cast<int>(g.unpacked.load() * 100 + 0.5));
      return line;
    case Stage::Launching: return g.detail.empty() ? L"Waiting for Roblox's window..." : g.detail;
    case Stage::Running: return g.detail.empty() ? L"Close Roblox to come back here." : g.detail;
    case Stage::Failed:
      std::swprintf(line, 160, L"It exited with code %lu. Try OpenGL ES in Settings if it keeps happening.", g.exit_code);
      return line;
    default: return g.detail;
  }
}

float main_content_height() {
  float h = 101;
  if (!status_line().empty()) h += 20;
  if (busy()) h += 22;
  if (g.stage == Stage::Failed && g.details) h += 11 + 16 + 15 * static_cast<float>(std::max<size_t>(1, g.error_lines.size())) + 7 + 15;
  return h;
}

void fit_main_window() {
  if (!g.main.hwnd) return;
  const float wanted = main_content_height() + kCommandHeight;
  if (std::fabs(wanted - g.main.height) < 0.5f) return;
  RECT frame = {0, 0, static_cast<LONG>(std::lround(kMainWidth * g.main.dpi / 96)), static_cast<LONG>(std::lround(wanted * g.main.dpi / 96))};
  const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(g.main.hwnd, GWL_STYLE));
  AdjustWindowRectEx(&frame, style, FALSE, 0);
  if (IsIconic(g.main.hwnd)) {
    WINDOWPLACEMENT placement{};
    placement.length = sizeof placement;
    GetWindowPlacement(g.main.hwnd, &placement);
    placement.rcNormalPosition.bottom = placement.rcNormalPosition.top + (frame.bottom - frame.top);
    SetWindowPlacement(g.main.hwnd, &placement);
    return;
  }
  SetWindowPos(g.main.hwnd, nullptr, 0, 0, frame.right - frame.left, frame.bottom - frame.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void draw_main() {
  const float w = c->width, content = main_content_height();
  fill_rect(D2D1::RectF(0, 0, w, content), ink(kWhite));
  draw_logo(31, 32, 32);
  const float x = 62, tw = w - x - kMargin;
  text(headline(), g.fonts.instruction, rect(x, 9, tw, 22), kInstruction);
  const std::pair<const wchar_t*, std::wstring> facts[] = {
      {L"Renderer:", g.backend},
      {L"Quality:", quality_name(g.settings.quality) + (runs_vulkan(g.settings.backend) && g.settings.render_scale < 100
                                                         ? L", " + std::to_wstring(g.settings.render_scale) + L"% resolution"
                                                         : L"")},
      {L"Device:", device_name(g.settings.device)}};
  float y = 37;
  for (const auto& [label, value] : facts) {
    text(label, g.fonts.body, rect(x, y, 64, 15), kSecondary);
    text(value, g.fonts.body, rect(x + 64, y, tw - 64, 15), kText);
    y += 17;
  }
  y += 9;
  const std::wstring status = status_line();
  if (!status.empty()) {
    text(status, g.fonts.body, rect(x, y, tw, 15), g.stage == Stage::Failed ? kError : kText);
    y += 20;
  }
  if (busy()) {
    progress_bar(rect(x, y, tw, 15), static_cast<float>(g.unpacked.load()), g.stage == Stage::Launching);
    y += 22;
  }
  if (g.stage == Stage::Failed && g.details) {
    y += 7;
    const float rows = static_cast<float>(std::max<size_t>(1, g.error_lines.size()));
    const D2D1_RECT_F box = D2D1::RectF(kMargin, y, w - kMargin, y + 16 + rows * 15);
    fill_rect(box, ink(0xFBFBFB));
    stroke_rect(box, 0xABADB3);
    float ly = box.top + 8;
    if (g.error_lines.empty()) text(L"(no session log yet)", g.fonts.mono, rect(box.left + 7, ly, box.right - box.left - 14, 15), kSecondary);
    for (const std::wstring& entry : g.error_lines) {
      text(entry, g.fonts.mono, rect(box.left + 7, ly, box.right - box.left - 14, 15), kText);
      ly += 15;
    }
    link(kLogsLink, kMargin, box.bottom + 7, L"Open the logs folder", [] {
      ShellExecuteW(g.main.hwnd, L"open", logs_dir(g.paths).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    });
  }

  const D2D1_RECT_F bar = D2D1::RectF(0, content, w, content + kCommandHeight);
  fill_rect(bar, ink(kFace));
  hline(0, w, bar.top, kSeparator);
  const float by = bar.top + kMargin;
  if (g.stage == Stage::Failed) {
    const int id = kDetailsButton;
    const float hv = hover(id);
    const D2D1_POINT_2F center = pt(kMargin + 9.5f, by + 11.5f);
    dot(center, 9.5f, 9.5f, gradient({{0, mix(0xFFFFFF, 0xEAF6FD, hv)}, {1, mix(0xE0E0E0, 0xA7D9F5, hv)}}, pt(0, by + 2), pt(0, by + 21)));
    ring(center, 9, ink(mix(0x8E8F8F, 0x3C7FB1, hv)));
    const float d = g.details ? -1.0f : 1.0f;
    path({pt(center.x - 3.5f, center.y - 1.5f * d), pt(center.x, center.y + 2 * d), pt(center.x + 3.5f, center.y - 1.5f * d)},
         ink(0x333333), 1.4f, false);
    const std::wstring label = g.details ? L"Hide details" : L"Show details";
    text(label, g.fonts.body, rect(kMargin + 25, by + 4, 120, 15), kText);
    clickable(id, rect(kMargin, by, text_width(label, g.fonts.body) + 27, 23), [] {
      g.details = !g.details;
      fit_main_window();
    });
  }
  const float right = w - kMargin;
  push_button(kSettingsButton, rect(right - kButtonWidth, by, kButtonWidth, kButtonHeight), L"Settings...", true,
              [] { open_settings(Tab::Graphics); });
  push_button(kPlayButton, rect(right - kButtonWidth * 2 - kGap, by, kButtonWidth, kButtonHeight),
              g.stage == Stage::Failed ? L"Try again" : L"Play", can_play(), launch, true);
}

void mark_dirty() { g.dirty = true; }

float combo_row(const std::wstring& label, float x, float y, float label_width, float field_width, Control control,
                const std::vector<std::wstring>& labels, size_t selected, bool enabled, std::function<void(size_t)> pick) {
  text(label, g.fonts.body, rect(x, y + 4, label_width - 5, 15), enabled ? kText : kDisabled);
  combo(control, rect(x + label_width, y, field_width, kButtonHeight), labels, selected, enabled, std::move(pick));
  return y + kButtonHeight;
}

std::wstring quality_note(const std::wstring& quality) {
  if (quality == L"performance") return L"Graphics level 1 plus Blox7's memory savers.";
  if (quality == L"balanced") return L"The middle of Roblox's slider, memory savers still on.";
  if (quality == L"high") return L"Level 17, full texture budgets, no forced low-end LOD.";
  if (quality == L"superhq") return L"Level 21, 4x MSAA and full-resolution textures.";
  return L"Roblox's own graphics slider decides, like on a phone.";
}

void page_graphics(const D2D1_RECT_F& page) {
  const float x = page.left + kMargin, w = page.right - page.left - kMargin * 2, ix = x + 9, iw = w - 18;
  float y = page.top + kMargin;
  const bool vulkan = uses_vulkan(g.pending.backend);
  group(rect(x, y, w, 68), L"Renderer");
  const wchar_t* vulkan_label = g.vulkan_driver ? L"Vulkan (recommended, fastest)" : L"Vulkan (no Vulkan driver found)";
  radio(kRenderer, 0, ix, y + 16, iw, vulkan, vulkan_label, [] {
    g.pending.backend = L"direct-vulkan";
    mark_dirty();
  });
  radio(kRenderer, 1, ix, y + 40, iw, !vulkan && uses_gles(g.pending.backend), L"OpenGL ES (for drivers that trip over Vulkan)", [] {
    g.pending.backend = L"opengl";
    mark_dirty();
  });
  y += 68 + kMargin;

  static const std::vector<std::wstring> qualities = {L"auto", L"performance", L"balanced", L"high", L"superhq"};
  group(rect(x, y, w, 96), L"Quality");
  combo_row(L"Preset:", ix, y + 16, 64, 200, kQuality, {L"Roblox decides", L"Performance", L"Balanced", L"High", L"Super HQ"},
            index_of(qualities, g.pending.quality), true, [](size_t i) {
              g.pending.quality = qualities[i];
              mark_dirty();
            });
  static const int scales[] = {100, 90, 80, 70, 60, 50};
  size_t scale = 0;
  for (size_t i = 0; i < std::size(scales); ++i)
    if (scales[i] == g.pending.render_scale) scale = i;
  combo_row(L"Resolution:", ix, y + 42, 64, 120, kRenderScale, {L"Native", L"90%", L"80%", L"70%", L"60%", L"50%"}, scale,
            runs_vulkan(g.pending.backend), [](size_t i) {
              g.pending.render_scale = scales[i];
              mark_dirty();
            });
  if (runs_vulkan(g.pending.backend) && g.pending.render_scale < 100) {
    const SIZE size = render_size(g.pending);
    text(L"3D at " + std::to_wstring(size.cx) + L" x " + std::to_wstring(size.cy), g.fonts.body,
         rect(ix + 194, y + 46, iw - 194, 15), kSecondary);
  }
  text(quality_note(g.pending.quality), g.fonts.body, rect(ix, y + 72, iw, 15), kSecondary);
  y += 96 + kMargin;

  std::vector<std::wstring> labels{L"Automatic"};
  size_t selected = g.pending.gpu.empty() ? 0 : g.gpus.size() + 1;
  for (size_t i = 0; i < g.gpus.size(); ++i) {
    labels.push_back(g.gpus[i].name);
    if (g.gpus[i].id == g.pending.gpu) selected = i + 1;
  }
  std::wstring note;
  const bool choice = g.gpus.size() > 1;
  if (!choice) {
    labels = {g.gpus.empty() ? L"No graphics card listed" : g.gpus[0].name};
    selected = 0;
  } else if (selected > g.gpus.size()) {
    note = L"The card picked earlier is gone, so Roblox picks one.";
  } else if (!vulkan && selected > 0 && g.os.build >= 17134) {
    note = L"Saved as Blox7's card in Windows' graphics settings.";
  }
  group(rect(x, y, w, note.empty() ? 50 : 72), L"Graphics card");
  combo_row(L"Use:", ix, y + 16, 64, iw - 64, kGpu, labels, selected, choice, [](size_t i) {
    g.pending.gpu = i == 0 ? std::wstring() : g.gpus[i - 1].id;
    mark_dirty();
  });
  if (!note.empty()) text(note, g.fonts.body, rect(ix, y + 46, iw, 15), kSecondary);
}

void page_display(const D2D1_RECT_F& page) {
  const float x = page.left + kMargin, w = page.right - page.left - kMargin * 2, ix = x + 9, iw = w - 18;
  float y = page.top + kMargin;
  std::vector<std::pair<int, int>> sizes;
  for (const auto& size : {std::pair{1280, 720}, std::pair{1366, 768}, std::pair{1600, 900}, std::pair{1920, 1080},
                           std::pair{2560, 1440}, std::pair{3840, 2160}})
    if (size.first <= g.screen_w && size.second <= g.screen_h) sizes.push_back(size);
  if (sizes.empty()) sizes.push_back({1280, 720});
  const std::pair<int, int> current{g.pending.width, g.pending.height};
  if (std::find(sizes.begin(), sizes.end(), current) == sizes.end()) sizes.push_back(current);
  std::vector<std::wstring> labels;
  for (const auto& size : sizes) labels.push_back(std::to_wstring(size.first) + L" x " + std::to_wstring(size.second));

  group(rect(x, y, w, 98), L"Window");
  combo_row(L"Size:", ix, y + 16, 92, 160, kWindowSize, labels,
            static_cast<size_t>(std::find(sizes.begin(), sizes.end(), current) - sizes.begin()), true, [sizes](size_t i) {
              g.pending.width = sizes[i].first;
              g.pending.height = sizes[i].second;
              mark_dirty();
            });
  checkbox(kFullscreen, ix, y + 46, iw, g.pending.fullscreen, L"Start Roblox in fullscreen", [] {
    g.pending.fullscreen = !g.pending.fullscreen;
    mark_dirty();
  });
  const int scale = static_cast<int>(std::lround(c->dpi * 100 / 96));
  checkbox(kHighDpi, ix, y + 70, iw, g.pending.high_dpi,
           scale > 100 ? L"Sharp picture on this " + std::to_wstring(scale) + L"% scaled screen" : L"Sharp picture on high-DPI screens",
           [] {
             g.pending.high_dpi = !g.pending.high_dpi;
             mark_dirty();
           });
  y += 98 + kMargin;

  const float fullscreen = g.pending.refresh_rate > 0 ? 72 : 50;
  group(rect(x, y, w, fullscreen), L"Fullscreen");
  std::vector<int> rates{0};
  for (const int hz : g.rates) rates.push_back(hz);
  if (std::find(rates.begin(), rates.end(), g.pending.refresh_rate) == rates.end()) rates.push_back(g.pending.refresh_rate);
  std::vector<std::wstring> names;
  for (const int hz : rates)
    names.push_back(hz == 0 ? L"Same as the desktop (" + std::to_wstring(g.monitor_hz) + L" Hz)" : std::to_wstring(hz) + L" Hz");
  combo_row(L"Refresh rate:", ix, y + 16, 92, 200, kRefresh, names,
            static_cast<size_t>(std::find(rates.begin(), rates.end(), g.pending.refresh_rate) - rates.begin()), true,
            [rates](size_t i) {
              g.pending.refresh_rate = rates[i];
              mark_dirty();
            });
  if (g.pending.refresh_rate > 0)
    text(L"Fullscreen switches the screen to this rate while Roblox runs.", g.fonts.body, rect(ix, y + 46, iw, 15), kSecondary);
  y += fullscreen + kMargin;

  group(rect(x, y, w, 44), L"Browser");
  checkbox(kJoinLinks, ix, y + 16, iw, g.pending.join_links, L"Join games from roblox.com in Blox7", [] {
    g.pending.join_links = !g.pending.join_links;
    mark_dirty();
  });
}

std::wstring frame_label(const std::wstring& value) {
  if (value == L"-1") return L"Roblox decides";
  if (value == L"unlimited") return L"No limit";
  if (value == L"display") return L"Match the display";
  return value + L" FPS";
}

void page_performance(const D2D1_RECT_F& page) {
  const float x = page.left + kMargin, w = page.right - page.left - kMargin * 2, ix = x + 9, iw = w - 18;
  float y = page.top + kMargin;
  std::vector<std::wstring> values = {L"-1", L"60", L"120", L"144", L"240", L"unlimited"};
  const auto add_number = [&values](int fps) {
    const std::wstring value = std::to_wstring(fps);
    if (fps <= 0 || std::find(values.begin(), values.end(), value) != values.end()) return;
    auto at = values.begin() + 1;
    while (at != values.end() - 1 && _wtoi(at->c_str()) < fps) ++at;
    values.insert(at, value);
  };
  add_number(g.monitor_hz);
  add_number(_wtoi(g.pending.frame_rate_limit.c_str()));
  if (g.pending.frame_rate_limit == L"display") values.insert(values.begin() + 1, L"display");
  std::vector<std::wstring> labels;
  for (const std::wstring& value : values) labels.push_back(frame_label(value));

  group(rect(x, y, w, 102), L"Frame rate");
  combo_row(L"Limit:", ix, y + 16, 64, 160, kFrameRate, labels, index_of(values, g.pending.frame_rate_limit), true,
            [values](size_t i) {
              g.pending.frame_rate_limit = values[i];
              mark_dirty();
            });
  static const std::vector<std::wstring> syncs = {L"auto", L"on", L"off"};
  combo_row(L"VSync:", ix, y + 46, 64, 160, kVsync, {L"Automatic", L"On", L"Off"}, index_of(syncs, g.pending.vsync), true,
            [](size_t i) {
              g.pending.vsync = syncs[i];
              mark_dirty();
            });
  const std::wstring& vsync = g.pending.vsync;
  text(vsync == L"on"    ? L"VSync on: no tearing, a little more latency."
       : vsync == L"off" ? L"VSync off: lowest latency, the picture can tear."
                         : L"Automatic follows the frame rate limit.",
       g.fonts.body, rect(ix, y + 76, iw, 15), kSecondary);
  y += 102 + kMargin;

  static const std::vector<std::wstring> workers = {L"auto", L"throughput", L"latency"};
  const std::wstring& mode = g.pending.worker_mode;
  group(rect(x, y, w, 96), L"Processor");
  combo_row(L"Workers:", ix, y + 16, 64, 160, kWorkers, {L"Roblox decides", L"Throughput", L"Latency"}, index_of(workers, mode),
            true, [](size_t i) {
              g.pending.worker_mode = workers[i];
              mark_dirty();
            });
  text(mode == L"auto"      ? L"Roblox sizes its own thread pools, like on a phone."
       : mode == L"latency" ? L"Roblox's own pools, with physics work batched up."
                            : L"Every CPU core works for Roblox. Best in busy servers.",
       g.fonts.body, rect(ix, y + 46, iw, 15), kSecondary);
  checkbox(kThreadedRendering, ix, y + 68, iw, g.pending.threaded_rendering, L"Multithreaded rendering", [] {
    g.pending.threaded_rendering = !g.pending.threaded_rendering;
    mark_dirty();
  });
}

void add_flag() {
  std::wstring name = g.name_text, value = g.value_text;
  while (!name.empty() && name.back() == L' ') name.pop_back();
  while (!name.empty() && name.front() == L' ') name.erase(0, 1);
  const std::wstring error = check_fast_flag(name, &value, g.pending);
  if (!error.empty()) {
    g.flag_message = error;
    g.flag_error = true;
    return;
  }
  for (FastFlag& flag : g.flags)
    if (flag.name == name) {
      if (flag.managed) {
        g.flag_message = L"Super HQ sets this flag. Change the preset first.";
        g.flag_error = true;
        return;
      }
      flag.value = value;
      g.flag_message = L"Changed " + name + L" to " + value + L".";
      g.flag_error = false;
      g.name_text.clear();
      g.value_text.clear();
      mark_dirty();
      return;
    }
  g.flags.push_back({name, value, false, {}});
  g.flag_selected = static_cast<int>(g.flags.size()) - 1;
  g.flag_message = L"Added " + name + L".";
  g.flag_error = false;
  g.name_text.clear();
  g.value_text.clear();
  g.focus = kNameField;
  g.caret = 0;
  mark_dirty();
}

void remove_flag() {
  if (g.flag_selected < 0 || g.flag_selected >= static_cast<int>(g.flags.size())) return;
  if (g.flags[g.flag_selected].managed) return;
  g.flag_message = L"Removed " + g.flags[g.flag_selected].name + L".";
  g.flag_error = false;
  g.flags.erase(g.flags.begin() + g.flag_selected);
  g.flag_selected = -1;
  mark_dirty();
}

bool run_file_dialog(OPENFILENAMEW& dialog, bool save) {
  using DialogFn = BOOL(WINAPI*)(LPOPENFILENAMEW);
  static HMODULE comdlg = LoadLibraryW(L"comdlg32.dll");
  const auto run = reinterpret_cast<DialogFn>(GetProcAddress(comdlg, save ? "GetSaveFileNameW" : "GetOpenFileNameW"));
  return run && run(&dialog);
}

std::wstring flags_file_dialog(bool save) {
  wchar_t file[MAX_PATH] = L"ClientAppSettings.json";
  OPENFILENAMEW dialog{};
  dialog.lStructSize = sizeof dialog;
  dialog.hwndOwner = g.dialog.hwnd;
  dialog.lpstrFilter = L"Flag files (*.json)\0*.json\0All files\0*.*\0";
  dialog.lpstrFile = file;
  dialog.nMaxFile = MAX_PATH;
  dialog.lpstrDefExt = L"json";
  dialog.lpstrTitle = save ? L"Export fast flags" : L"Import fast flags";
  dialog.Flags = OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_EXPLORER | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
  if (!run_file_dialog(dialog, save)) return {};
  return file;
}

void import_flags() {
  const std::wstring file = flags_file_dialog(false);
  if (file.empty()) return;
  const size_t before = g.flags.size();
  bool failed = false;
  const std::vector<FastFlag> old = g.flags;
  g.flag_message = import_fast_flags(file, &g.flags, g.pending, &failed);
  g.flag_error = failed;
  if (g.flags.size() != before || g.flags.size() != old.size() ||
      !std::equal(g.flags.begin(), g.flags.end(), old.begin(), [](const FastFlag& a, const FastFlag& b) { return a.value == b.value; }))
    mark_dirty();
}

void export_flags() {
  const std::wstring file = flags_file_dialog(true);
  if (file.empty()) return;
  const std::wstring error = export_fast_flags(file, g.flags);
  g.flag_error = !error.empty();
  g.flag_message = error.empty() ? L"Exported to " + file.substr(file.find_last_of(L"\\/") + 1) + L"." : error;
}

bool flag_matches(const FastFlag& flag) {
  if (g.find_text.empty()) return true;
  const auto lower = [](std::wstring text) {
    for (wchar_t& ch : text) ch = static_cast<wchar_t>(towlower(ch));
    return text;
  };
  return lower(flag.name).find(lower(g.find_text)) != std::wstring::npos;
}

void page_flags(const D2D1_RECT_F& page) {
  const float x = page.left + kMargin, w = page.right - page.left - kMargin * 2;
  float y = page.top + kMargin;
  text(L"Find:", g.fonts.body, rect(x, y + 4, 40, 15), kText);
  text_field(kFindField, rect(x + 38, y, w - 38 - kButtonWidth * 2 - kGap * 2, kButtonHeight), g.find_text);
  push_button(kImportFlags, rect(x + w - kButtonWidth * 2 - kGap, y, kButtonWidth, kButtonHeight), L"Import...", true, import_flags);
  push_button(kExportFlags, rect(x + w - kButtonWidth, y, kButtonWidth, kButtonHeight), L"Export...", !g.flags.empty(), export_flags);
  y += kButtonHeight + kGap;

  constexpr float header = 22, row = 20;
  const int visible = 6;
  const D2D1_RECT_F list = rect(x, y, w, header + row * visible + 2);
  g.flag_list_area = list;
  fill_rect(list, ink(kWhite));
  stroke_rect(list, kListEdge);
  const float name_width = (w - 2) * 0.66f;
  fill_vgrad(D2D1::RectF(list.left + 1, list.top + 1, list.right - 1, list.top + header), 0, {{0, kWhite}, {1, 0xF6F7F9}});
  hline(list.left + 1, list.right - 1, list.top + header - 1, 0xE5E5E5);
  vline(list.left + 1 + name_width, list.top + 4, list.top + header - 4, 0xE3E8EC);
  text(L"Name", g.fonts.field, D2D1::RectF(list.left + 7, list.top + 1, list.left + name_width, list.top + header), kHeaderText);
  text(L"Value", g.fonts.field, D2D1::RectF(list.left + name_width + 8, list.top + 1, list.right - 6, list.top + header), kHeaderText);

  std::vector<int> shown;
  for (int i = 0; i < static_cast<int>(g.flags.size()); ++i)
    if (flag_matches(g.flags[i])) shown.push_back(i);
  const int count = static_cast<int>(shown.size());
  g.flag_scroll = std::clamp(g.flag_scroll, 0, std::max(0, count - visible));
  const bool scrolling = count > visible;
  const float rows_right = scrolling ? list.right - 12 : list.right - 2;
  if (count == 0)
    text(g.flags.empty() ? L"No fast flags yet. Add one below." : L"No flag matches.", g.fonts.centered,
         D2D1::RectF(list.left, list.top + header, list.right, list.bottom - 1), kSecondary);
  for (int k = g.flag_scroll; k < std::min(count, g.flag_scroll + visible); ++k) {
    const int i = shown[k];
    const FastFlag& flag = g.flags[i];
    const float ry = list.top + header + 1 + row * static_cast<float>(k - g.flag_scroll);
    const D2D1_RECT_F item = D2D1::RectF(list.left + 2, ry, rows_right, ry + row - 1);
    const int id = kFlagRowFirst + (k - g.flag_scroll);
    explorer_highlight(item, g.flag_selected == i, g.flag_selected == i ? 0 : hover(id));
    const uint32_t ink_color = flag.managed ? kDisabled : kText;
    text(flag.name, g.fonts.field, D2D1::RectF(item.left + 5, item.top, list.left + name_width - 4, item.bottom), ink_color);
    text(flag.managed ? flag.value + L" (" + flag.owner + L")" : flag.value, g.fonts.field,
         D2D1::RectF(list.left + name_width + 8, item.top, item.right - 4, item.bottom), ink_color);
    clickable(id, item, [i] {
      g.flag_selected = i;
      g.flag_message.clear();
      if (!g.flags[i].managed) {
        g.name_text = g.flags[i].name;
        g.value_text = g.flags[i].value;
      }
    });
  }
  if (scrolling) {
    const D2D1_RECT_F track = D2D1::RectF(list.right - 11, list.top + header + 1, list.right - 2, list.bottom - 2);
    fill_rect(track, ink(0xF4F4F4));
    const float span = track.bottom - track.top, size = std::max(18.0f, span * visible / count);
    const float top = track.top + (span - size) * g.flag_scroll / std::max(1, count - visible);
    fill_round(D2D1::RectF(track.left + 1, top + 1, track.right - 1, top + size - 1), 2,
               gradient({{0, 0xF3F3F3}, {1, 0xD9D9D9}}, pt(track.left, 0), pt(track.right, 0)));
    stroke_round(D2D1::RectF(track.left + 1.5f, top + 1.5f, track.right - 1.5f, top + size - 1.5f), 2, 0x9A9A9A);
  }
  y = list.bottom + kMargin;

  text(L"Name:", g.fonts.body, rect(x, y + 4, 40, 15), kText);
  text_field(kNameField, rect(x + 38, y, 168, kButtonHeight), g.name_text);
  text(L"Value:", g.fonts.body, rect(x + 217, y + 4, 40, 15), kText);
  text_field(kValueField, rect(x + 255, y, w - 255 - 58, kButtonHeight), g.value_text);
  push_button(kAddFlag, rect(x + w - 51, y, 51, kButtonHeight), L"Add", !g.name_text.empty(), add_flag);
  y += kButtonHeight + kGap;

  const bool removable = g.flag_selected >= 0 && g.flag_selected < static_cast<int>(g.flags.size()) &&
                         !g.flags[g.flag_selected].managed;
  if (!g.flag_message.empty()) text(g.flag_message, g.fonts.body, rect(x, y + 4, w - kButtonWidth - kGap, 15), g.flag_error ? kError : kSecondary);
  push_button(kRemoveFlag, rect(x + w - kButtonWidth, y, kButtonWidth, kButtonHeight), L"Remove", removable, remove_flag);

  const float wy = page.bottom - kMargin - 15;
  warning_sign(x, wy + 1);
  text(L"Some flags send capability bits to Roblox. Use at your own risk.", g.fonts.body, rect(x + 21, wy, w - 21, 15),
       kSecondary);
}

Glyph device_glyph(DeviceKind kind) {
  switch (kind) {
    case DeviceKind::kTablet: return Glyph::Tablet;
    case DeviceKind::kPc: return Glyph::Pc;
    case DeviceKind::kConsole: return Glyph::Console;
    default: return Glyph::Phone;
  }
}

void page_device(const D2D1_RECT_F& page) {
  const float x = page.left + kMargin, w = page.right - page.left - kMargin * 2;
  float y = page.top + kMargin;
  const std::wstring current = canonical_device(g.pending.device);
  bool known = false;
  for (const DeviceChoice& device : g.devices) known |= device.id == current;
  text(known ? L"Roblox sees Blox7 as this device."
             : L"config.yaml asks for \"" + g.pending.device + L"\". Picking a device here replaces it.",
       g.fonts.body, rect(x, y, w, 15), kText);
  y += 15 + kGap;
  constexpr float tile_height = 36, gap = 4;
  const float tile_width = (w - gap) / 2;
  for (size_t i = 0; i < g.devices.size(); ++i) {
    const DeviceChoice& device = g.devices[i];
    const D2D1_RECT_F tile = rect(x + (tile_width + gap) * static_cast<float>(i % 2),
                                  y + (tile_height + gap) * static_cast<float>(i / 2), tile_width, tile_height);
    const bool checked = device.id == current;
    const int id = option_id(kProfile, i);
    explorer_highlight(tile, checked, checked ? 0 : hover(id));
    draw_glyph(device_glyph(device.kind), tile.left + 5, tile.top + 4, 28);
    text(device.name, g.fonts.body, D2D1::RectF(tile.left + 38, tile.top + 3, tile.right - 4, tile.top + 18), kText);
    text(device.detail, g.fonts.body, D2D1::RectF(tile.left + 38, tile.top + 18, tile.right - 4, tile.top + 33), kSecondary);
    if (!checked)
      clickable(id, tile, [i] {
        g.pending.device = g.devices[i].id;
        mark_dirty();
      });
  }
}

void page_about(const D2D1_RECT_F& page) {
  const float x = page.left + kMargin, w = page.right - page.left - kMargin * 2;
  float y = page.top + kMargin;
  draw_logo(x + 24, y + 24, 36);
  text(L"Blox7", g.fonts.instruction, rect(x + 60, y, w - 60, 21), kInstruction);
  text(L"Version 0.1", g.fonts.body, rect(x + 60, y + 24, w - 60, 15), kSecondary);
  text(L"Roblox ported back to Windows 7.", g.fonts.body, rect(x + 60, y + 39, w - 60, 15), kText);
  y += 66;
  text(L"Runs Roblox's Android app natively on Windows 7 through 11. Built on Mocktail by komaruworld.",
       g.fonts.paragraph, rect(x, y, w, 32), kText);
  y += 40;
  text(L"Blox7 is not affiliated with or endorsed by Roblox Corporation or Microsoft. Roblox and the Roblox logo are "
       L"trademarks of Roblox Corporation. Windows is a trademark of Microsoft.",
       g.fonts.paragraph, rect(x, y, w, 46), kSecondary);
  y += 58;
  link(kDataLink, x, y, L"Open the data folder", [] { ShellExecuteW(g.dialog.hwnd, L"open", g.paths.data.c_str(), nullptr, nullptr, SW_SHOWNORMAL); });
  link(kLogsLink, x + 140, y, L"Open the logs folder",
       [] { ShellExecuteW(g.dialog.hwnd, L"open", logs_dir(g.paths).c_str(), nullptr, nullptr, SW_SHOWNORMAL); });
  link(kLicensesLink, x + 280, y, L"View licenses", [] {
    const std::wstring notices = g.paths.runtime + L"\\THIRD-PARTY-NOTICES.txt";
    ShellExecuteW(g.dialog.hwnd, L"open", notices.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
  });

  const float before = text_width(L"Made with ", g.fonts.body), after = text_width(L" by Zeph", g.fonts.body);
  const float left = std::round(x + (w - before - 11 - after) / 2), cy = page.bottom - kMargin - 15;
  text(L"Made with ", g.fonts.body, rect(left, cy, before + 2, 15), kSecondary);
  heart(left + before + 5.5f, cy + 8);
  text(L" by Zeph", g.fonts.body, rect(left + before + 11, cy, after + 2, 15), kSecondary);
}

void slider(Control control, float x, float y, float w, float value, float lo, float hi, bool enabled,
            const std::function<void(float)>& set) {
  const int id = option_id(control, 0);
  const float left = x + 6, right = x + w - 6;
  const float t = std::clamp((value - lo) / (hi - lo), 0.0f, 1.0f);
  const D2D1_RECT_F channel = D2D1::RectF(left - 2, y + 7, right + 2, y + 11);
  fill_rect(channel, ink(0xE7EAEA));
  stroke_rect(channel, 0xB0B5BA);
  for (int i = 0; i <= 10; ++i)
    vline(std::round(left + (right - left) * static_cast<float>(i) / 10), y + 19, y + 22, enabled ? 0xA0A0A0 : 0xCCCCCC);
  const float cx = std::round(left + (right - left) * t) + 0.5f;
  const Look look = !enabled ? Look::Disabled : c->pressed == id ? Look::Pressed : Look::Normal;
  static const uint32_t faces[4][3] = {
      {0x707070, 0xF2F2F2, 0xD6D6D6}, {0x3C7FB1, 0xEAF6FD, 0xA7D9F5}, {0x2C628B, 0xC4E5F6, 0x68B3DB}, {0xADB2B5, 0xF4F4F4, 0xF4F4F4}};
  const auto thumb = [&](const uint32_t* face, float alpha) {
    ID2D1LinearGradientBrush* fill = gradient({{0, face[1]}, {1, face[2]}}, pt(0, y + 1), pt(0, y + 17));
    if (fill) fill->SetOpacity(alpha);
    path({pt(cx - 5, y + 1), pt(cx + 5, y + 1), pt(cx + 5, y + 13), pt(cx, y + 18), pt(cx - 5, y + 13)}, fill, 0, true, true);
    path({pt(cx - 5, y + 1), pt(cx + 5, y + 1), pt(cx + 5, y + 13), pt(cx, y + 18), pt(cx - 5, y + 13)}, ink(face[0], alpha), 1,
         true);
  };
  thumb(faces[static_cast<int>(look)], 1);
  if (look == Look::Normal) {
    const float hv = hover(id);
    if (hv > 0.01f) thumb(faces[1], hv);
  }
  if (enabled)
    draggable(id, D2D1::RectF(x, y - 2, x + w, y + 22), [left, right, lo, hi, set](float mx) {
      set(lo + std::clamp((mx - left) / (right - left), 0.0f, 1.0f) * (hi - lo));
    });
}

void browse_shader() {
  wchar_t file[MAX_PATH] = {};
  if (!g.pending.shader.empty()) wcsncpy(file, g.pending.shader.c_str(), MAX_PATH - 1);
  const std::wstring folder = g.paths.runtime + L"\\share\\blox7\\shaders";
  OPENFILENAMEW dialog{};
  dialog.lStructSize = sizeof dialog;
  dialog.hwndOwner = g.dialog.hwnd;
  dialog.lpstrFilter = L"Shaders (*.frag, *.glsl)\0*.frag;*.glsl\0All files\0*.*\0";
  dialog.lpstrFile = file;
  dialog.nMaxFile = MAX_PATH;
  dialog.lpstrInitialDir = folder.c_str();
  dialog.lpstrTitle = L"Pick a custom shader";
  dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_EXPLORER;
  if (!run_file_dialog(dialog, false)) return;
  g.pending.shader = file;
  const std::wstring error = compile_shader(g.paths, file);
  g.shader_error = !error.empty();
  g.shader_status = error.empty() ? L"Compiled. It replaces the built-in effects and gets the slider values." : error;
  mark_dirty();
}

void page_effects(const D2D1_RECT_F& page) {
  const float x = page.left + kMargin, w = page.right - page.left - kMargin * 2, ix = x + 9, iw = w - 18;
  float y = page.top + kMargin;
  const bool vulkan = runs_vulkan(g.pending.backend);
  static const std::vector<std::wstring> presets = {L"off", L"crisp", L"vivid", L"cinematic", L"custom"};
  const float look = vulkan ? 158 : 177;
  group(rect(x, y, w, look), L"Look");
  combo_row(L"Preset:", ix, y + 16, 80, 120, kEffects, {L"Off", L"Crisp", L"Vivid", L"Cinematic", L"Custom"},
            index_of(presets, g.pending.effects), vulkan, [](size_t i) {
              effects_preset(&g.pending, presets[i]);
              mark_dirty();
            });
  checkbox(kFxaa, ix + 214, y + 19, iw - 214, g.pending.fxaa, L"Smooth edges (FXAA)", [] {
    g.pending.fxaa = !g.pending.fxaa;
    mark_dirty();
  }, vulkan);
  struct Row {
    const wchar_t* label;
    Control control;
    float* value;
    float lo, hi, scale;
  };
  const Row rows[] = {{L"Sharpening:", kSharpen, &g.pending.sharpen, 0, 1, 100},
                      {L"Vibrance:", kVibrance, &g.pending.vibrance, -1, 1, 100},
                      {L"Contrast:", kContrast, &g.pending.contrast, 0.5f, 1.5f, 100},
                      {L"Brightness:", kBrightness, &g.pending.brightness, -0.25f, 0.25f, 400}};
  float ry = y + 46;
  for (const Row& row : rows) {
    text(row.label, g.fonts.body, rect(ix, ry + 3, 80, 15), vulkan ? kText : kDisabled);
    slider(row.control, ix + 80, ry, iw - 80 - 48, *row.value, row.lo, row.hi, vulkan, [value = row.value](float v) {
      *value = std::round(v * 100) / 100;
      g.pending.effects = L"custom";
      mark_dirty();
    });
    wchar_t shown[16];
    const int number = static_cast<int>(std::lround(*row.value * row.scale));
    std::swprintf(shown, 16, row.lo < 0 && number > 0 ? L"+%d%%" : L"%d%%", number);
    text(shown, g.fonts.body, rect(ix + iw - 42, ry + 3, 42, 15), vulkan ? kText : kDisabled);
    ry += 26;
  }
  if (!vulkan) text(L"Effects need the Vulkan renderer (Graphics tab).", g.fonts.body, rect(ix, ry + 2, iw, 15), kSecondary);
  y += look + kMargin;

  group(rect(x, y, w, 72), L"Custom shader");
  const D2D1_RECT_F box = rect(ix, y + 16, iw - kButtonWidth * 2 - kGap * 2, kButtonHeight);
  fill_rect(box, ink(vulkan ? kWhite : 0xF4F4F4));
  stroke_rect(box, 0xABADB3);
  const size_t slash = g.pending.shader.find_last_of(L"\\/");
  const std::wstring name = g.pending.shader.empty() ? L"(none)"
                            : slash == std::wstring::npos ? g.pending.shader
                                                          : g.pending.shader.substr(slash + 1);
  text(name, g.fonts.field, D2D1::RectF(box.left + 5, box.top, box.right - 5, box.bottom), g.pending.shader.empty() ? kDisabled : kText);
  push_button(kBrowseShader, rect(box.right + kGap, y + 16, kButtonWidth, kButtonHeight), L"Browse...", vulkan, browse_shader);
  push_button(kRemoveShader, rect(box.right + kGap * 2 + kButtonWidth, y + 16, kButtonWidth, kButtonHeight), L"Remove",
              vulkan && !g.pending.shader.empty(), [] {
                g.pending.shader.clear();
                g.shader_status.clear();
                g.shader_error = false;
                mark_dirty();
              });
  const std::wstring status = !g.shader_status.empty() ? g.shader_status
                              : g.pending.shader.empty() ? L"Your own .frag shader can replace the built-in effects."
                                                         : L"It replaces the built-in effects and gets the slider values.";
  text(status, g.fonts.body, rect(ix, y + 46, iw, 15), g.shader_error ? kError : kSecondary);
}

void draw_tabs(const D2D1_RECT_F& strip) {
  float x = strip.left + 2;
  D2D1_RECT_F chosen{};
  for (size_t i = 0; i < std::size(kTabs); ++i) {
    const float w = text_width(kTabs[i].label, g.fonts.body) + 12;
    const bool selected = g.tab == kTabs[i].tab;
    const int id = kTabFirst + static_cast<int>(i);
    if (selected) {
      chosen = D2D1::RectF(x - 2, strip.top, x + w + 2, strip.bottom + 1);
    } else {
      const D2D1_RECT_F r = D2D1::RectF(x, strip.top + 2, x + w, strip.bottom);
      fill_vgrad(r, 0, {{0, 0xF3F3F3}, {0.5f, 0xEBEBEB}, {0.5f, 0xDDDDDD}, {1, 0xCFCFCF}});
      const float hv = hover(id);
      if (hv > 0.01f) fill_vgrad(r, 0, {{0, 0xEAF6FD}, {0.5f, 0xD9F0FC}, {0.5f, 0xBEE6FD}, {1, 0xA7D9F5}}, hv);
      hline(r.left, r.right, r.top, mix(kTabEdge, 0x3C7FB1, hv));
      vline(r.left, r.top, r.bottom, mix(kTabEdge, 0x3C7FB1, hv));
      vline(r.right - 1, r.top, r.bottom, mix(kTabEdge, 0x3C7FB1, hv));
      text(kTabs[i].label, g.fonts.centered, D2D1::RectF(r.left, r.top + 1, r.right, r.bottom), kText);
      clickable(id, r, [tab = kTabs[i].tab] {
        g.tab = tab;
        g.focus = 0;
      });
    }
    x += w;
  }
  if (chosen.right > chosen.left) {
    fill_rect(chosen, ink(kWhite));
    hline(chosen.left, chosen.right, chosen.top, kTabEdge);
    vline(chosen.left, chosen.top, chosen.bottom, kTabEdge);
    vline(chosen.right - 1, chosen.top, chosen.bottom, kTabEdge);
    for (size_t i = 0; i < std::size(kTabs); ++i)
      if (g.tab == kTabs[i].tab)
        text(kTabs[i].label, g.fonts.centered, D2D1::RectF(chosen.left, chosen.top, chosen.right, chosen.bottom - 2), kText);
  }
}

void draw_dialog() {
  const float w = c->width, h = c->height;
  fill_rect(D2D1::RectF(0, 0, w, h), ink(kFace));
  const D2D1_RECT_F strip = D2D1::RectF(kMargin, kMargin, w - kMargin, kMargin + kTabHeight);
  const D2D1_RECT_F page = D2D1::RectF(kMargin, strip.bottom, w - kMargin, h - kMargin - kButtonHeight - kMargin);
  fill_rect(page, ink(kWhite));
  stroke_rect(page, kTabEdge);
  draw_tabs(strip);
  const D2D1_RECT_F inner = inset(page, 1);
  switch (g.tab) {
    case Tab::Graphics: page_graphics(inner); break;
    case Tab::Display: page_display(inner); break;
    case Tab::Effects: page_effects(inner); break;
    case Tab::Performance: page_performance(inner); break;
    case Tab::Flags: page_flags(inner); break;
    case Tab::Device: page_device(inner); break;
    case Tab::About: page_about(inner); break;
  }
  const float by = h - kMargin - kButtonHeight, right = w - kMargin;
  push_button(kApplyButton, rect(right - kButtonWidth, by, kButtonWidth, kButtonHeight), L"Apply", g.dirty, apply_settings);
  push_button(kCancelButton, rect(right - kButtonWidth * 2 - kGap, by, kButtonWidth, kButtonHeight), L"Cancel", true, close_settings);
  push_button(kOkButton, rect(right - kButtonWidth * 3 - kGap * 2, by, kButtonWidth, kButtonHeight), L"OK", true, [] {
    apply_settings();
    if (!g.dirty) close_settings();
  }, true);
}

HRESULT draw_frame() {
  c->hits.clear();
  c->popup.active = false;
  c->popup.pick = nullptr;
  for (auto& entry : c->springs)
    if (entry.first < kStateFirst) entry.second.target = 0;
  c->rt->BeginDraw();
  c->rt->SetTransform(D2D1::Matrix3x2F::Identity());
  if (c->draw) c->draw();
  draw_popup();
  return c->rt->EndDraw();
}

void paint() {
  g.clock = static_cast<float>(GetTickCount64() % 3600000) / 1000.0f;
  if (!create_device(*c)) return;
  if (draw_frame() == static_cast<HRESULT>(D2DERR_RECREATE_TARGET)) drop_device(*c);
  sync_timer();
}

void repaint(Canvas& canvas) {
  if (canvas.hwnd) InvalidateRect(canvas.hwnd, nullptr, FALSE);
}

void apply_settings() {
  if (!g.dirty) return;
  std::wstring error = save_settings(g.paths, g.pending);
  if (error.empty()) error = save_fast_flags(g.paths, g.flags);
  if (!error.empty()) {
    MessageBoxW(g.dialog.hwnd, (L"Blox7 couldn't save the settings: " + error).c_str(), L"Blox7", MB_ICONERROR);
    return;
  }
  g.settings = g.pending;
  g.flags = load_fast_flags(g.paths);
  g.dirty = false;
  refresh_info();
  repaint(g.main);
}

void close_settings() {
  if (!g.dialog.hwnd) return;
  EnableWindow(g.main.hwnd, TRUE);
  SetForegroundWindow(g.main.hwnd);
  DestroyWindow(g.dialog.hwnd);
}

void start_unpack() {
  g.stage = Stage::Unpacking;
  g.unpacked = 0;
  fit_main_window();
  repaint(g.main);
  std::thread([] {
    std::wstring error = install_runtime(g.paths, [](double done) {
      g.unpacked = done;
      PostMessageW(g.main.hwnd, kUnpackProgress, 0, 0);
    });
    const bool installed = error.empty();
    if (installed) prepare_data(g.paths);
    auto* result = new std::wstring(std::move(error));
    if (!PostMessageW(g.main.hwnd, kUnpackDone, 0, reinterpret_cast<LPARAM>(result))) delete result;
    if (installed) remove_old_runtimes(g.paths);
  }).detach();
}

BOOL CALLBACK find_window(HWND hwnd, LPARAM param) {
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  if (pid == static_cast<DWORD>(param) && IsWindowVisible(hwnd) && !GetWindow(hwnd, GW_OWNER)) {
    SetLastError(ERROR_SUCCESS);
    return FALSE;
  }
  return TRUE;
}

bool child_has_window() {
  SetLastError(ERROR_INVALID_DATA);
  return !EnumWindows(find_window, static_cast<LPARAM>(g.child.dwProcessId)) && GetLastError() == ERROR_SUCCESS;
}

void launch() {
  if (!can_play()) return;
  if (!runtime_ready(g.paths)) {
    start_unpack();
    return;
  }
  std::wstring command = L"\"" + g.paths.exe + L"\" --play";
  STARTUPINFOW startup{};
  startup.cb = sizeof startup;
  if (!CreateProcessW(g.paths.exe.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &g.child)) {
    g.stage = Stage::Failed;
    g.exit_code = GetLastError();
    g.error_lines = {L"Blox7 could not start its own play process."};
    fit_main_window();
    return;
  }
  GetSystemTimeAsFileTime(&g.launched);
  g.window_seen = false;
  g.detail.clear();
  g.error_lines.clear();
  g.details = false;
  g.stage = Stage::Launching;
  SetTimer(g.main.hwnd, kWatchTimer, 250, nullptr);
  fit_main_window();
  repaint(g.main);
}

void watch_shader() {
  static int tick = 0;
  static FILETIME failed{};
  if (g.stage != Stage::Running || ++tick % 4 || g.settings.shader.empty() || !runs_vulkan(g.settings.backend)) return;
  WIN32_FILE_ATTRIBUTE_DATA source{};
  if (!GetFileAttributesExW(g.settings.shader.c_str(), GetFileExInfoStandard, &source) ||
      CompareFileTime(&source.ftLastWriteTime, &failed) == 0)
    return;
  bool rebuilt = false;
  const std::wstring error = refresh_shader(g.paths, g.settings.shader, &rebuilt);
  if (!error.empty()) {
    failed = source.ftLastWriteTime;
    g.detail = L"Shader: " + error;
  } else if (rebuilt) {
    g.detail = L"Rebuilt the custom shader, Roblox switches to it in a moment.";
  } else {
    return;
  }
  repaint(g.main);
}

void watch_child() {
  if (!g.child.hProcess) {
    KillTimer(g.main.hwnd, kWatchTimer);
    return;
  }
  watch_shader();
  if (!g.window_seen && child_has_window()) {
    g.window_seen = true;
    g.detail.clear();
    g.stage = Stage::Running;
    if (!IsIconic(g.main.hwnd)) {
      ShowWindow(g.main.hwnd, SW_MINIMIZE);
      g.minimized_by_us = true;
    }
    fit_main_window();
  }
  if (WaitForSingleObject(g.child.hProcess, 0) == WAIT_OBJECT_0) {
    DWORD code = 0;
    GetExitCodeProcess(g.child.hProcess, &code);
    CloseHandle(g.child.hProcess);
    CloseHandle(g.child.hThread);
    g.child = {};
    KillTimer(g.main.hwnd, kWatchTimer);
    g.exit_code = code;
    if (!g.window_seen && code != 0) {
      g.stage = Stage::Failed;
      g.error_lines = log_tail(g.paths, 10);
    } else {
      g.stage = Stage::Ready;
      g.detail = g.window_seen ? L"Roblox closed. See you next time." : L"Roblox closed before it opened a window.";
    }
    if (g.minimized_by_us) {
      ShowWindow(g.main.hwnd, SW_RESTORE);
      SetForegroundWindow(g.main.hwnd);
      g.minimized_by_us = false;
    }
    refresh_info();
    g.settings = load_settings(g.paths);
    fit_main_window();
  } else if (g.stage == Stage::Launching) {
    const UpdateState update = update_state(g.paths);
    if (CompareFileTime(&update.written, &g.launched) > 0) {
      if (!update.message.empty()) g.detail = update.message;
      else if (update.phase == L"checking") g.detail = L"Checking for a newer Roblox...";
      else if (update.phase == L"downloading") g.detail = L"Downloading Roblox, about 150 MB the first time...";
      else if (update.phase == L"verifying") g.detail = L"Testing the new Roblox before switching to it...";
    }
  }
  repaint(g.main);
}

Canvas* canvas_of(HWND hwnd) { return reinterpret_cast<Canvas*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA)); }

void track_mouse(LPARAM lp) {
  c->mouse_x = static_cast<float>(static_cast<short>(LOWORD(lp))) * 96.0f / c->dpi;
  c->mouse_y = static_cast<float>(static_cast<short>(HIWORD(lp))) * 96.0f / c->dpi;
}

void mouse_moved(LPARAM lp) {
  track_mouse(lp);
  const Hit* over = hit_at(c->mouse_x, c->mouse_y);
  const int hot = over ? over->id : 0;
  if (hot != c->hot) {
    c->hot = hot;
    InvalidateRect(c->hwnd, nullptr, FALSE);
  }
  if (!c->tracking) {
    TRACKMOUSEEVENT track{sizeof track, TME_LEAVE, c->hwnd, 0};
    c->tracking = TrackMouseEvent(&track);
  }
}

std::wstring* focused_text() {
  if (c != &g.dialog || g.tab != Tab::Flags) return nullptr;
  if (g.focus == kNameField) return &g.name_text;
  if (g.focus == kValueField) return &g.value_text;
  if (g.focus == kFindField) return &g.find_text;
  return nullptr;
}

void restart_caret() {
  g.caret_on = true;
  if (g.dialog.hwnd) SetTimer(g.dialog.hwnd, kCaretTimer, GetCaretBlinkTime(), nullptr);
}

bool edit_key(WPARAM key) {
  std::wstring* field = focused_text();
  if (!field) return false;
  g.caret = std::min(g.caret, field->size());
  const bool control = GetKeyState(VK_CONTROL) < 0;
  switch (key) {
    case VK_LEFT: if (g.caret > 0) --g.caret; break;
    case VK_RIGHT: if (g.caret < field->size()) ++g.caret; break;
    case VK_HOME: g.caret = 0; break;
    case VK_END: g.caret = field->size(); break;
    case VK_DELETE: if (g.caret < field->size()) field->erase(g.caret, 1); break;
    case 'V':
      if (!control || !OpenClipboard(c->hwnd)) return control;
      if (HANDLE data = GetClipboardData(CF_UNICODETEXT)) {
        if (const wchar_t* pasted = static_cast<const wchar_t*>(GlobalLock(data))) {
          std::wstring clean;
          for (const wchar_t* p = pasted; *p && clean.size() < 200; ++p)
            if (*p >= 0x20 && *p < 0x7F) clean += *p;
          field->insert(g.caret, clean);
          g.caret += clean.size();
          GlobalUnlock(data);
        }
      }
      CloseClipboard();
      break;
    default: return false;
  }
  restart_caret();
  return true;
}

bool edit_char(wchar_t ch) {
  std::wstring* field = focused_text();
  if (!field) return false;
  g.caret = std::min(g.caret, field->size());
  if (ch == L'\b') {
    if (g.caret > 0) field->erase(--g.caret, 1);
  } else if (ch == L'\t') {
    g.focus = g.focus == kFindField ? kNameField : g.focus == kNameField ? kValueField : kFindField;
    g.caret = focused_text()->size();
  } else if (ch == L'\r') {
    if (g.focus != kFindField && !g.name_text.empty()) add_flag();
  } else if (ch == 27) {
    g.focus = 0;
  } else if (ch >= 0x20 && ch < 0x7F && field->size() < 200) {
    field->insert(g.caret++, 1, ch);
  } else {
    return ch < 0x20;
  }
  restart_caret();
  return true;
}

UINT window_dpi(HWND hwnd) {
  using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
  static auto get = reinterpret_cast<GetDpiForWindowFn>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
  if (get && hwnd) return get(hwnd);
  HDC screen = GetDC(nullptr);
  const UINT dpi = static_cast<UINT>(GetDeviceCaps(screen, LOGPIXELSX));
  ReleaseDC(nullptr, screen);
  return dpi ? dpi : 96;
}

LRESULT handle(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
  switch (message) {
    case WM_PAINT: {
      PAINTSTRUCT ps;
      BeginPaint(hwnd, &ps);
      paint();
      EndPaint(hwnd, &ps);
      return 0;
    }
    case WM_ERASEBKGND:
      return 1;
    case WM_SIZE:
      if (wp == SIZE_MINIMIZED) return 0;
      c->width = static_cast<float>(LOWORD(lp)) * 96.0f / c->dpi;
      c->height = static_cast<float>(HIWORD(lp)) * 96.0f / c->dpi;
      if (c->window_rt) c->window_rt->Resize(D2D1::SizeU(LOWORD(lp), HIWORD(lp)));
      InvalidateRect(hwnd, nullptr, FALSE);
      return 0;
    case WM_DPICHANGED: {
      c->dpi = static_cast<float>(HIWORD(wp));
      const RECT* suggested = reinterpret_cast<const RECT*>(lp);
      SetWindowPos(hwnd, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                   suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
      if (c->rt) c->rt->SetDpi(c->dpi, c->dpi);
      return 0;
    }
    case WM_ACTIVATE:
      c->active = LOWORD(wp) != WA_INACTIVE;
      InvalidateRect(hwnd, nullptr, FALSE);
      break;
    case WM_MOUSEMOVE:
      mouse_moved(lp);
      if (c->drag && c->pressed) {
        const std::function<void(float)> drag = c->drag;
        drag(c->mouse_x);
        InvalidateRect(hwnd, nullptr, FALSE);
      }
      return 0;
    case WM_MOUSELEAVE:
      c->tracking = false;
      c->hot = 0;
      InvalidateRect(hwnd, nullptr, FALSE);
      return 0;
    case WM_MOUSEWHEEL:
      if (c == &g.dialog && g.tab == Tab::Flags && inside(g.flag_list_area, c->mouse_x, c->mouse_y)) {
        g.flag_scroll -= GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA * 3;
        InvalidateRect(hwnd, nullptr, FALSE);
      }
      return 0;
    case WM_LBUTTONDOWN: {
      track_mouse(lp);
      const Hit* hit = hit_at(c->mouse_x, c->mouse_y);
      c->pressed = hit ? hit->id : 0;
      c->drag = hit ? hit->drag : nullptr;
      if (c->drag) c->drag(c->mouse_x);
      if (c == &g.dialog && (!hit || (hit->id != kNameField && hit->id != kValueField && hit->id != kFindField &&
                                      hit->id != kAddFlag)))
        g.focus = 0;
      if (c->pressed) SetCapture(hwnd);
      InvalidateRect(hwnd, nullptr, FALSE);
      return 0;
    }
    case WM_LBUTTONUP:
      track_mouse(lp);
      if (c->pressed) {
        ReleaseCapture();
        const int pressed = c->pressed;
        c->pressed = 0;
        c->drag = nullptr;
        const Hit* hit = hit_at(c->mouse_x, c->mouse_y);
        if (hit && hit->id == pressed && hit->action) {
          const std::function<void()> action = hit->action;
          action();
          if (g.focus) restart_caret();
        }
        if (IsWindow(hwnd)) InvalidateRect(hwnd, nullptr, FALSE);
      }
      return 0;
    case WM_SETCURSOR:
      if (LOWORD(lp) == HTCLIENT) {
        const Hit* hit = hit_at(c->mouse_x, c->mouse_y);
        const Cursor cursor = hit ? hit->cursor : Cursor::Arrow;
        SetCursor(LoadCursorW(nullptr, cursor == Cursor::Hand ? IDC_HAND : cursor == Cursor::Beam ? IDC_IBEAM : IDC_ARROW));
        return TRUE;
      }
      break;
    case WM_CHAR:
      if (edit_char(static_cast<wchar_t>(wp))) {
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
      }
      break;
    case WM_KEYDOWN:
      if (edit_key(wp)) {
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
      }
      if (focused_text() && (wp == VK_RETURN || wp == VK_ESCAPE || wp == VK_TAB)) return 0;
      if (wp == VK_ESCAPE) {
        if (c->open_combo >= 0) c->open_combo = -1;
        else if (c == &g.dialog) close_settings();
      } else if (wp == VK_RETURN) {
        if (c == &g.main) launch();
        else if (c == &g.dialog && c->open_combo < 0) {
          apply_settings();
          if (!g.dirty) close_settings();
        }
      }
      if (IsWindow(hwnd)) InvalidateRect(hwnd, nullptr, FALSE);
      return 0;
    case WM_SYSCOMMAND:
      if ((wp & 0xFFF0) == kAboutCommand) {
        open_settings(Tab::About);
        return 0;
      }
      break;
    case WM_TIMER:
      if (wp == kAnimTimer) step_animation();
      else if (wp == kWatchTimer) watch_child();
      else if (wp == kCaretTimer) {
        g.caret_on = !g.caret_on;
        if (!g.focus) KillTimer(hwnd, kCaretTimer);
        InvalidateRect(hwnd, nullptr, FALSE);
      }
      return 0;
    case kUnpackProgress:
      InvalidateRect(hwnd, nullptr, FALSE);
      return 0;
    case kUnpackDone: {
      std::wstring* error = reinterpret_cast<std::wstring*>(lp);
      if (error->empty()) {
        g.stage = Stage::Ready;
        g.detail.clear();
        refresh_info();
      } else {
        g.stage = Stage::Failed;
        g.exit_code = 0;
        g.error_lines = {*error};
      }
      delete error;
      fit_main_window();
      InvalidateRect(hwnd, nullptr, FALSE);
      return 0;
    }
    case WM_CLOSE:
      if (c == &g.dialog) {
        close_settings();
        return 0;
      }
      break;
    case WM_DESTROY:
      drop_device(*c);
      KillTimer(hwnd, kAnimTimer);
      KillTimer(hwnd, kCaretTimer);
      c->timer_on = false;
      c->hwnd = nullptr;
      c->hits.clear();
      c->springs.clear();
      c->open_combo = -1;
      if (c == &g.main) PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(hwnd, message, wp, lp);
}

LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
  if (message == WM_NCCREATE) {
    Canvas* canvas = static_cast<Canvas*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(canvas));
    canvas->hwnd = hwnd;
  }
  Canvas* canvas = canvas_of(hwnd);
  if (!canvas) return DefWindowProcW(hwnd, message, wp, lp);
  Canvas* previous = c;
  c = canvas;
  const LRESULT result = handle(hwnd, message, wp, lp);
  c = previous;
  return result;
}

HWND create_window(Canvas& canvas, const wchar_t* klass, const wchar_t* title, DWORD style, DWORD ex_style, float width,
                   float height, HWND owner) {
  canvas.dpi = static_cast<float>(window_dpi(owner));
  RECT frame = {0, 0, static_cast<LONG>(std::lround(width * canvas.dpi / 96)), static_cast<LONG>(std::lround(height * canvas.dpi / 96))};
  AdjustWindowRectEx(&frame, style, FALSE, ex_style);
  const int fw = frame.right - frame.left, fh = frame.bottom - frame.top;
  RECT area;
  if (owner) GetWindowRect(owner, &area);
  else SystemParametersInfoW(SPI_GETWORKAREA, 0, &area, 0);
  int x = area.left + (area.right - area.left - fw) / 2, y = area.top + (area.bottom - area.top - fh) / 2;
  if (owner) {
    RECT work;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    x = std::clamp(x, static_cast<int>(work.left), std::max(static_cast<int>(work.left), static_cast<int>(work.right) - fw));
    y = std::clamp(y, static_cast<int>(work.top), std::max(static_cast<int>(work.top), static_cast<int>(work.bottom) - fh));
  }
  HWND hwnd = CreateWindowExW(ex_style, klass, title, style, x, y, fw, fh, owner, nullptr, GetModuleHandleW(nullptr), &canvas);
  if (!hwnd) return nullptr;
  const float actual = static_cast<float>(window_dpi(hwnd));
  if (actual != canvas.dpi) {
    canvas.dpi = actual;
    RECT fix = {0, 0, static_cast<LONG>(std::lround(width * actual / 96)), static_cast<LONG>(std::lround(height * actual / 96))};
    AdjustWindowRectEx(&fix, style, FALSE, ex_style);
    SetWindowPos(hwnd, nullptr, 0, 0, fix.right - fix.left, fix.bottom - fix.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
  }
  return hwnd;
}

void open_settings(Tab tab) {
  if (g.dialog.hwnd) {
    g.tab = tab;
    SetForegroundWindow(g.dialog.hwnd);
    repaint(g.dialog);
    return;
  }
  g.tab = tab;
  g.pending = g.settings;
  g.flags = load_fast_flags(g.paths);
  g.rates = refresh_rates();
  g.dirty = false;
  g.focus = 0;
  g.flag_selected = -1;
  g.flag_scroll = 0;
  g.flag_message.clear();
  g.shader_status.clear();
  g.shader_error = false;
  g.name_text.clear();
  g.value_text.clear();
  g.dialog.draw = draw_dialog;
  HWND hwnd = create_window(g.dialog, kDialogClass, L"Blox7 Settings", WS_POPUP | WS_CAPTION | WS_SYSMENU, WS_EX_DLGMODALFRAME,
                            kDialogWidth, kDialogHeight, g.main.hwnd);
  if (!hwnd) return;
  EnableWindow(g.main.hwnd, FALSE);
  ShowWindow(hwnd, SW_SHOW);
}

bool init_graphics() {
  HMODULE d2d1 = LoadLibraryW(L"d2d1.dll");
  HMODULE dwrite = LoadLibraryW(L"dwrite.dll");
  using CreateD2D = HRESULT(WINAPI*)(D2D1_FACTORY_TYPE, REFIID, const D2D1_FACTORY_OPTIONS*, void**);
  using CreateDWrite = HRESULT(WINAPI*)(DWRITE_FACTORY_TYPE, REFIID, IUnknown**);
  auto create_d2d = d2d1 ? reinterpret_cast<CreateD2D>(GetProcAddress(d2d1, "D2D1CreateFactory")) : nullptr;
  auto create_dw = dwrite ? reinterpret_cast<CreateDWrite>(GetProcAddress(dwrite, "DWriteCreateFactory")) : nullptr;
  if (!create_d2d || !create_dw ||
      FAILED(create_d2d(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory), nullptr, reinterpret_cast<void**>(&g.d2d))) ||
      FAILED(create_dw(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(&g.dw)))) {
    return false;
  }
  const D2D1_STROKE_STYLE_PROPERTIES round =
      D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND);
  g.d2d->CreateStrokeStyle(round, nullptr, 0, &g.round);
  build_logo();
  create_fonts();
  return true;
}

void read_machine() {
  g.os = os_info();
  g.settings = load_settings(g.paths);
  g.pending = g.settings;
  g.gpus = gpu_choices();
  g.devices = device_choices();
  g.rates = refresh_rates();
  DEVMODEW mode{};
  mode.dmSize = sizeof mode;
  if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &mode)) {
    if (mode.dmDisplayFrequency > 1) g.monitor_hz = static_cast<int>(mode.dmDisplayFrequency);
    g.screen_w = static_cast<int>(mode.dmPelsWidth);
    g.screen_h = static_cast<int>(mode.dmPelsHeight);
  }
}

const GUID kPngFormat = {0x1b7cfaf4, 0x713f, 0x473c, {0xbb, 0xcd, 0x61, 0x37, 0x42, 0x5f, 0xae, 0xaf}};
const GUID kPremultipliedBgra = {0x6fddc324, 0x4e03, 0x4bfe, {0xb1, 0x85, 0x3d, 0x77, 0x76, 0x8d, 0xc9, 0x10}};

bool save_png(IWICImagingFactory* wic, IWICBitmap* bitmap, const std::wstring& file) {
  IWICStream* stream = nullptr;
  IWICBitmapEncoder* encoder = nullptr;
  IWICBitmapFrameEncode* frame = nullptr;
  bool ok = SUCCEEDED(wic->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromFilename(file.c_str(), GENERIC_WRITE)) &&
            SUCCEEDED(wic->CreateEncoder(kPngFormat, nullptr, &encoder)) && SUCCEEDED(encoder->Initialize(stream, WICBitmapEncoderNoCache)) &&
            SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) &&
            SUCCEEDED(frame->WriteSource(bitmap, nullptr)) && SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
  release(frame);
  release(encoder);
  release(stream);
  return ok;
}

bool render_canvas(IWICImagingFactory* wic, Canvas& canvas, float scale, const std::wstring& file) {
  IWICBitmap* bitmap = nullptr;
  if (FAILED(wic->CreateBitmap(static_cast<UINT>(std::lround(canvas.width * scale)), static_cast<UINT>(std::lround(canvas.height * scale)),
                               kPremultipliedBgra, WICBitmapCacheOnLoad, &bitmap)))
    return false;
  const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
      D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96 * scale, 96 * scale);
  bool ok = false;
  canvas.dpi = 96 * scale;
  if (SUCCEEDED(g.d2d->CreateWicBitmapRenderTarget(bitmap, props, &canvas.rt)) &&
      SUCCEEDED(canvas.rt->CreateSolidColorBrush(rgb(kText), &canvas.brush))) {
    Canvas* previous = c;
    c = &canvas;
    draw_frame();
    c = previous;
    ok = save_png(wic, bitmap, file);
  }
  for (auto& entry : canvas.gradients) release(entry.second);
  canvas.gradients.clear();
  release(canvas.brush);
  release(canvas.rt);
  release(bitmap);
  return ok;
}

}  // namespace

int render_ui(const std::wstring& dir) {
  if (!init_graphics()) return 1;
  HMODULE ole = LoadLibraryW(L"ole32.dll");
  using CoInit = HRESULT(WINAPI*)(LPVOID, DWORD);
  using CoCreate = HRESULT(WINAPI*)(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID*);
  auto init = ole ? reinterpret_cast<CoInit>(GetProcAddress(ole, "CoInitializeEx")) : nullptr;
  auto create = ole ? reinterpret_cast<CoCreate>(GetProcAddress(ole, "CoCreateInstance")) : nullptr;
  if (!init || !create) return 1;
  init(nullptr, COINIT_APARTMENTTHREADED);
  static const CLSID kWicFactory = {0xcacaf262, 0x9370, 0x4615, {0xa1, 0x3b, 0x9f, 0x55, 0x39, 0xda, 0x4c, 0x0a}};
  IWICImagingFactory* wic = nullptr;
  if (FAILED(create(kWicFactory, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IWICImagingFactory), reinterpret_cast<void**>(&wic)))) return 1;
  g.paths = resolve_paths();
  read_machine();
  g.settings = Settings{};
  g.settings.quality = L"superhq";
  g.settings.frame_rate_limit = L"144";
  g.settings.render_scale = 80;
  effects_preset(&g.settings, L"vivid");
  g.pending = g.settings;
  g.version = L"2.746.1266";
  g.backend = L"Vulkan";
  g.error_lines = {L"[engine] joining startup thread with timeout 120000ms",
                   L"[runtime] could not open the Vulkan driver: VK_ERROR_INCOMPATIBLE_DRIVER",
                   L"[FATAL] no graphics backend could start"};
  g.flags = {{L"FIntDebugForceMSAASamples", L"4", true, L"Super HQ"},
             {L"DFIntTextureQualityOverride", L"3", true, L"Super HQ"},
             {L"DFFlagTextureQualityOverrideEnabled", L"True", true, L"Super HQ"},
             {L"FFlagDebugGraphicsPreferD3D11", L"False", false, {}},
             {L"FIntFRMMinGrassDistance", L"0", false, {}}};
  CreateDirectoryW(dir.c_str(), nullptr);
  g.main.draw = draw_main;
  g.dialog.draw = draw_dialog;
  int written = 0;
  struct MainShot {
    const wchar_t* name;
    Stage stage;
    bool details;
    int hot;
  } mains[] = {{L"main-ready", Stage::Ready, false, kPlayButton},
               {L"main-unpacking", Stage::Unpacking, false, 0},
               {L"main-launching", Stage::Launching, false, 0},
               {L"main-running", Stage::Running, false, 0},
               {L"main-failed", Stage::Failed, true, kDetailsButton}};
  struct DialogShot {
    const wchar_t* name;
    Tab tab;
    int hot;
    int open;
  } dialogs[] = {{L"settings-graphics", Tab::Graphics, kTabFirst + 1, -1},
                 {L"settings-graphics-open", Tab::Graphics, kPopupFirst + 3, kQuality},
                 {L"settings-display", Tab::Display, option_id(kFullscreen, 0), -1},
                 {L"settings-effects", Tab::Effects, option_id(kVibrance, 0), -1},
                 {L"settings-performance", Tab::Performance, kOkButton, -1},
                 {L"settings-flags", Tab::Flags, kFlagRowFirst + 3, -1},
                 {L"settings-device", Tab::Device, option_id(kProfile, 1), -1},
                 {L"settings-about", Tab::About, kDataLink, -1}};
  for (const float scale : {1.0f, 1.5f}) {
    for (const MainShot& shot : mains) {
      g.stage = shot.stage;
      g.details = shot.details;
      g.unpacked = 0.42;
      g.clock = 0.9f;
      g.exit_code = 1;
      g.detail = shot.stage == Stage::Launching ? L"Checking for a newer Roblox..." : L"";
      g.main.springs.clear();
      g.main.hot = shot.hot;
      g.main.active = true;
      g.main.width = kMainWidth;
      g.main.height = main_content_height() + kCommandHeight;
      wchar_t file[64];
      std::swprintf(file, 64, L"\\%ls@%gx.png", shot.name, scale);
      written += render_canvas(wic, g.main, scale, dir + file);
    }
    g.stage = Stage::Ready;
    for (const DialogShot& shot : dialogs) {
      g.tab = shot.tab;
      g.dialog.springs.clear();
      g.dialog.hot = shot.hot;
      g.dialog.open_combo = shot.open;
      g.dialog.active = true;
      g.dirty = shot.tab == Tab::Flags;
      g.flag_selected = shot.tab == Tab::Flags ? 3 : -1;
      g.name_text = shot.tab == Tab::Flags ? L"FFlagDebugGraphicsPreferD3D11" : L"";
      g.value_text = shot.tab == Tab::Flags ? L"False" : L"";
      g.focus = shot.tab == Tab::Flags ? kValueField : 0;
      g.caret = g.value_text.size();
      g.caret_on = true;
      g.flag_message = shot.tab == Tab::Flags ? L"Blox7 sets this flag itself, so it can't be added." : L"";
      g.flag_error = shot.tab == Tab::Flags;
      g.dialog.width = kDialogWidth;
      g.dialog.height = kDialogHeight;
      wchar_t file[64];
      std::swprintf(file, 64, L"\\%ls@%gx.png", shot.name, scale);
      written += render_canvas(wic, g.dialog, scale, dir + file);
    }
  }
  release(wic);
  return written ? 0 : 1;
}

int run_launcher(HINSTANCE instance, int show) {
  HANDLE single = CreateMutexW(nullptr, TRUE, L"Local\\Blox7.Launcher");
  if (single && GetLastError() == ERROR_ALREADY_EXISTS) {
    if (HWND other = FindWindowW(kMainClass, nullptr)) {
      ShowWindow(other, SW_RESTORE);
      SetForegroundWindow(other);
    }
    return 0;
  }
  if (!init_graphics()) {
    MessageBoxW(nullptr, L"Blox7 needs Direct2D (Windows 7 SP1 or newer).", L"Blox7", MB_ICONERROR);
    return 1;
  }
  g.paths = resolve_paths();
  read_machine();
  refresh_info();
  refresh_join_links(g.paths);

  WNDCLASSEXW wc{};
  wc.cbSize = sizeof wc;
  wc.lpfnWndProc = window_proc;
  wc.hInstance = instance;
  wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
  wc.hIconSm = wc.hIcon;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.lpszClassName = kMainClass;
  RegisterClassExW(&wc);
  wc.hIcon = nullptr;
  wc.hIconSm = nullptr;
  wc.lpszClassName = kDialogClass;
  RegisterClassExW(&wc);

  g.main.draw = draw_main;
  if (!runtime_ready(g.paths)) g.stage = Stage::Unpacking;
  HWND hwnd = create_window(g.main, kMainClass, L"Blox7", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, 0, kMainWidth,
                            main_content_height() + kCommandHeight, nullptr);
  if (!hwnd) return 1;
  if (HMENU menu = GetSystemMenu(hwnd, FALSE)) {
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kAboutCommand, L"About Blox7...");
  }
  ShowWindow(hwnd, show == SW_HIDE ? SW_SHOWNORMAL : show);
  UpdateWindow(hwnd);

  if (g.stage == Stage::Unpacking) start_unpack();
  else std::thread([] {
    prepare_data(g.paths);
    remove_old_runtimes(g.paths);
  }).detach();

  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  return static_cast<int>(msg.wParam);
}

}  // namespace blox7

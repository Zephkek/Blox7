#include <d2d1.h>
#include <dwrite.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
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
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "app.h"

namespace studio7 {
namespace {

constexpr wchar_t kMainClass[] = L"Studio7.Launcher";
constexpr wchar_t kDialogClass[] = L"Studio7.Settings";
constexpr UINT kProgress = WM_APP + 1;
constexpr UINT kJobDone = WM_APP + 2;
constexpr UINT kSelfUpdated = WM_APP + 3;
constexpr UINT_PTR kAnimTimer = 1;
constexpr UINT_PTR kWatchTimer = 2;
constexpr UINT kAboutCommand = 0x0010;

constexpr float kMargin = 11, kMainWidth = 460, kCommandHeight = 45;
constexpr float kDialogWidth = 490, kDialogHeight = 340, kTabHeight = 21;
constexpr float kButtonWidth = 75, kButtonHeight = 23, kGap = 7;

constexpr uint32_t kWhite = 0xFFFFFF, kFace = 0xF0F0F0, kSeparator = 0xDFDFDF;
constexpr uint32_t kInstruction = 0x003399, kText = 0x000000, kSecondary = 0x5D5D5D, kDisabled = 0x838383;
constexpr uint32_t kLink = 0x0066CC, kLinkHot = 0x3399FF, kError = 0xC42B1C;
constexpr uint32_t kGroupEdge = 0xD5DFE5, kTabEdge = 0x898C95;

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

enum class Stage { Fresh, Setting, Ready, Starting, Running, Failed, Unsupported };
enum class Tab { General, Mods, About };
enum class Look { Normal, Hot, Pressed, Disabled };
enum class Cursor { Arrow, Hand };
enum class Job { Check, Get };

enum : int {
  kOpenButton = 1,
  kSettingsButton,
  kDetailsButton,
  kOkButton,
  kCancelButton,
  kApplyButton,
  kCheckButton,
  kRemoveButton,
  kUpdatesBox,
  kLinksBox,
  kWebviewLink,
  kFolderLink,
  kLogLink,
  kLicensesLink,
  kArrowBrowse,
  kArrowRemove,
  kHandBrowse,
  kHandRemove,
  kFontBrowse,
  kFontRemove,
  kIconBrowse,
  kIconRemove,
  kModsLink,
  kSelfBox,
  kTabFirst = 100,
};

constexpr struct {
  Tab tab;
  const wchar_t* label;
} kTabs[] = {{Tab::General, L"General"}, {Tab::Mods, L"Mods"}, {Tab::About, L"About"}};

struct Spring {
  float value = 0, target = 0;
};

struct Hit {
  D2D1_RECT_F area;
  int id;
  std::function<void()> action;
  Cursor cursor;
};

struct Stop {
  float at;
  uint32_t color;
  float alpha = 1;
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
  int hot = 0, pressed = 0;
  float mouse_x = -1, mouse_y = -1;
  bool tracking = false, timer_on = false, active = false;
  ULONGLONG last_tick = 0;
  void (*draw)() = nullptr;
};

struct Outcome {
  Job job = Job::Check;
  int answer = CHECK_FAILED, result = SETUP_OK;
  Release latest{};
  std::wstring error;
};

struct App {
  Paths paths{};
  ID2D1Factory* d2d = nullptr;
  IDWriteFactory* dw = nullptr;
  ID2D1StrokeStyle* round = nullptr;
  ID2D1PathGeometry* logo[4] = {};
  Fonts fonts;
  Canvas main, dialog;

  Stage stage = Stage::Fresh;
  std::wstring version, webview, detail, updates, failure, offer, newer;
  std::vector<std::wstring> error_lines;
  bool details = false, failed_start = false, automatic = true;
  float clock = 0;

  std::mutex lock;
  std::wstring job_text;
  std::atomic<double> fraction{-1};
  std::atomic<bool> stop{false}, working{false}, quiet{false};
  bool install_after = false, asked = false;

  std::wstring arguments;
  bool open_when_ready = false;
  Run run{};
  bool minimized_by_us = false;

  Tab tab = Tab::General;
  bool dirty = false, pending_updates = true, pending_links = false, pending_self = true;
  std::wstring pending_mods[MOD_COUNT], mod_error[MOD_COUNT];
  HANDLE single = nullptr;
};

App& g = *new App;
Canvas* c = &g.main;
std::unordered_map<std::wstring, std::wstring> g_fitted;

void act();
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


float text_width(const std::wstring& s, IDWriteTextFormat* format) {
  IDWriteTextLayout* layout = nullptr;
  DWRITE_TEXT_METRICS metrics{};
  if (format && SUCCEEDED(g.dw->CreateTextLayout(s.c_str(), static_cast<UINT32>(s.size()), format, 4000, 200, &layout))) {
    layout->GetMetrics(&metrics);
    layout->Release();
  }
  return metrics.widthIncludingTrailingWhitespace;
}

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

D2D1_MATRIX_3X2_F tilt() {
  const float a = 15 * 3.14159265f / 180;
  return D2D1::Matrix3x2F(std::cos(a), std::sin(a), -std::sin(a), std::cos(a), 0, 0);
}

void draw_logo(float cx, float cy, float size) {
  static const uint32_t light[4] = {0x9ADCFF, 0x62C0FF, 0x4AA3F5, 0x3C7BD9};
  static const uint32_t deep[4] = {0x48B2F2, 0x1E8FE6, 0x1569C7, 0x0E47A1};
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

float spring(int key, float target) {
  Spring& s = c->springs.try_emplace(key, Spring{target, target}).first->second;
  s.target = target;
  return s.value;
}

float hover(int id) { return smooth(spring(id, c->hot == id ? 1.0f : 0.0f)); }

void clickable(int id, const D2D1_RECT_F& area, std::function<void()> action, Cursor cursor = Cursor::Arrow) {
  c->hits.push_back({area, id, std::move(action), cursor});
}

const Hit* hit_at(float x, float y) {
  for (auto it = c->hits.rbegin(); it != c->hits.rend(); ++it)
    if (inside(it->area, x, y)) return &*it;
  return nullptr;
}

bool busy() { return g.stage == Stage::Setting || g.stage == Stage::Starting; }
bool can_act() { return g.stage == Stage::Fresh || g.stage == Stage::Setting || g.stage == Stage::Ready || g.stage == Stage::Failed; }

bool animating() {
  if (c == &g.main && (busy() || (can_act() && c->active))) return true;
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

void check_mark(float x, float y, bool checked, float hv) {
  fill_rect(D2D1::RectF(x, y, x + 13, y + 13), ink(kWhite));
  const D2D1_RECT_F inner = D2D1::RectF(x + 2, y + 2, x + 11, y + 11);
  fill_vgrad(inner, 0, {{0, 0xCBCFD5}, {1, 0xF6F6F6}});
  if (hv > 0.01f) fill_vgrad(inner, 0, {{0, 0xB1DFFD}, {1, 0xE9F7FE}}, hv);
  stroke_rect(D2D1::RectF(x, y, x + 13, y + 13), mix(0x8E8F8F, 0x3C7FB1, hv));
  if (checked) path({pt(x + 3, y + 6.8f), pt(x + 5.6f, y + 9.4f), pt(x + 10.2f, y + 3.6f)}, ink(0x33415E), 1.8f, false);
}

void checkbox(int id, float x, float y, float w, bool checked, const std::wstring& label, std::function<void()> flip) {
  check_mark(x, y + 2, checked, hover(id));
  text(label, g.fonts.body, rect(x + 18, y + 1, w - 18, 15), kText);
  clickable(id, rect(x, y, std::min(w, text_width(label, g.fonts.body) + 22), 17), std::move(flip));
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

void refresh_info() {
  char version[96], name[64];
  wchar_t webview[64];
  g.version.clear();
  if (studio_current(&g.paths, version, sizeof version)) {
    state_get(&g.paths, L"name", name, sizeof name);
    g.version = widen(name);
  }
  g.webview = webview_version(webview, 64) ? webview : L"";
  g.automatic = updates_automatic(&g.paths);
}

std::wstring megabytes(ULONG64 bytes) { return std::to_wstring((bytes + 524288) / 1048576) + L" MB"; }

std::wstring job_text() {
  std::lock_guard<std::mutex> hold(g.lock);
  return g.job_text;
}

std::wstring headline() {
  switch (g.stage) {
    case Stage::Fresh: return L"Ready to download Roblox Studio";
    case Stage::Setting: return L"Setting up Roblox Studio";
    case Stage::Starting: return L"Starting Roblox Studio";
    case Stage::Running: return L"Roblox Studio is running";
    case Stage::Failed: return g.failed_start ? L"Roblox Studio didn't start" : L"Roblox Studio couldn't be set up";
    case Stage::Unsupported: return L"Studio7 is for Windows 7";
    default: return L"Roblox Studio " + g.version + L" is ready";
  }
}

std::wstring status_line() {
  switch (g.stage) {
    case Stage::Fresh: return g.detail.empty() ? g.offer : g.detail;
    case Stage::Setting: {
      const std::wstring text = job_text();
      return g.stop ? L"Stopping..." : text.empty() ? L"Getting ready..." : text;
    }
    case Stage::Starting: return L"Waiting for Roblox Studio's window...";
    case Stage::Running: return g.detail.empty() ? L"Close Roblox Studio to come back here." : g.detail;
    case Stage::Failed: return g.failure;
    default: return g.detail;
  }
}

std::wstring updates_line() {
  if (g.stage == Stage::Unsupported) return L"Off";
  if (g.working && g.quiet) {
    const std::wstring text = job_text();
    if (!text.empty()) return text;
  }
  if (!g.updates.empty()) return g.updates;
  return g.automatic ? L"Automatic" : L"Off";
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
      {L"Studio:", g.version.empty() ? L"Not installed yet" : g.version},
      {L"Updates:", updates_line()},
      {L"Sign-in:", g.webview.empty() ? L"The WebView2 Runtime is missing." : L"WebView2 " + g.webview}};
  float y = 37;
  for (const auto& [label, value] : facts) {
    text(label, g.fonts.body, rect(x, y, 64, 15), kSecondary);
    text(value, g.fonts.body, rect(x + 64, y, tw - 64, 15), kText);
    y += 17;
  }
  if (g.webview.empty())
    link(kWebviewLink, x + 64 + text_width(facts[2].second, g.fonts.body) + 6, y - 17, L"Get version 109", [] {
      ShellExecuteW(g.main.hwnd, L"open", L"https://www.catalog.update.microsoft.com/Search.aspx?q=WebView2%20Runtime%20109",
                    nullptr, nullptr, SW_SHOWNORMAL);
    });
  y += 9;
  const std::wstring status = status_line();
  if (!status.empty()) {
    text(status, g.fonts.body, rect(x, y, tw, 15), g.stage == Stage::Failed ? kError : kText);
    y += 20;
  }
  if (busy()) {
    const double fraction = g.fraction.load();
    progress_bar(rect(x, y, tw, 15), static_cast<float>(fraction), g.stage == Stage::Starting || fraction < 0);
    y += 22;
  }
  if (g.stage == Stage::Failed && g.details) {
    y += 7;
    const float rows = static_cast<float>(std::max<size_t>(1, g.error_lines.size()));
    const D2D1_RECT_F box = D2D1::RectF(kMargin, y, w - kMargin, y + 16 + rows * 15);
    fill_rect(box, ink(0xFBFBFB));
    stroke_rect(box, 0xABADB3);
    float ly = box.top + 8;
    if (g.error_lines.empty()) text(L"(the log is empty)", g.fonts.mono, rect(box.left + 7, ly, box.right - box.left - 14, 15), kSecondary);
    for (const std::wstring& entry : g.error_lines) {
      text(entry, g.fonts.mono, rect(box.left + 7, ly, box.right - box.left - 14, 15), kText);
      ly += 15;
    }
    link(kLogLink, kMargin, box.bottom + 7, L"Open the log",
         [] { ShellExecuteW(g.main.hwnd, L"open", g.paths.log, nullptr, nullptr, SW_SHOWNORMAL); });
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
              [] { open_settings(Tab::General); });
  const wchar_t* label = g.stage == Stage::Fresh     ? L"Install"
                         : g.stage == Stage::Setting ? L"Cancel"
                         : g.stage == Stage::Failed  ? L"Try again"
                                                     : L"Open Studio";
  push_button(kOpenButton, rect(right - kButtonWidth * 2 - kGap, by, kButtonWidth, kButtonHeight), label,
              can_act() && !(g.stage == Stage::Setting && g.stop), act, g.stage != Stage::Setting);
}

void mark_dirty() { g.dirty = true; }
void start_job(Job job, bool quiet);
void remove_all();

void page_general(const D2D1_RECT_F& page) {
  const float x = page.left + kMargin, w = page.right - page.left - kMargin * 2, ix = x + 9, iw = w - 18;
  float y = page.top + kMargin;
  const bool usable = g.stage != Stage::Unsupported;

  group(rect(x, y, w, 96), L"Roblox Studio");
  text(L"Installed:", g.fonts.body, rect(ix, y + 20, 64, 15), kSecondary);
  text(g.version.empty() ? L"Not installed yet" : g.version, g.fonts.body, rect(ix + 64, y + 20, iw - 64 - 92, 15), kText);
  push_button(kCheckButton, rect(ix + iw - 85, y + 16, 85, kButtonHeight), g.newer.empty() ? L"Check now" : L"Update now",
              usable && !g.working && !g.version.empty(), [] {
                studio_forgive(&g.paths);
                g.asked = !g.newer.empty();
                g.updates = g.asked ? L"" : L"Checking...";
                start_job(Job::Check, true);
              });
  checkbox(kUpdatesBox, ix, y + 46, iw, g.pending_updates, L"Get new versions of Roblox Studio as they come out", [] {
    g.pending_updates = !g.pending_updates;
    mark_dirty();
  });
  text(g.version.empty() ? L"An open Studio is never replaced, the new version starts next time."
                         : L"Updates: " + updates_line(),
       g.fonts.body, rect(ix, y + 70, iw, 15), kSecondary);
  y += 96 + kMargin;

  group(rect(x, y, w, 66), L"Browser");
  checkbox(kLinksBox, ix, y + 16, iw, g.pending_links, L"Open Roblox Studio links from roblox.com with Studio7", [] {
    g.pending_links = !g.pending_links;
    mark_dirty();
  });
  text(L"Edit buttons on the website and signing in through a browser use these.", g.fonts.body, rect(ix, y + 40, iw, 15), kSecondary);
  y += 66 + kMargin;

  group(rect(x, y, w, 52), L"Remove");
  text(L"Places, plugins and Studio's own settings stay.", g.fonts.body, rect(ix, y + 24, iw - 96, 15), kSecondary);
  push_button(kRemoveButton, rect(ix + iw - 85, y + 20, 85, kButtonHeight), L"Remove...",
              !g.working && g.stage != Stage::Starting && g.stage != Stage::Running, remove_all);
}

struct ModPicker {
  const wchar_t* label;
  int browse, remove;
  const wchar_t* none;
  const wchar_t* filter;
  const wchar_t* title;
  std::wstring (*check)(const std::wstring&);
};

const ModPicker kModPickers[MOD_COUNT] = {
    {L"Arrow cursor:", kArrowBrowse, kArrowRemove, L"Studio's arrow", L"PNG images (*.png)\0*.png\0",
     L"Pick an arrow cursor (a PNG with the tip in the middle)", check_cursor},
    {L"Hand cursor:", kHandBrowse, kHandRemove, L"Studio's hand", L"PNG images (*.png)\0*.png\0",
     L"Pick a hand cursor (a PNG with the fingertip in the middle)", check_cursor},
    {L"Font:", kFontBrowse, kFontRemove, L"Studio's font", L"Fonts (*.ttf, *.otf)\0*.ttf;*.otf\0",
     L"Pick a font for Studio's toolbar", check_font},
    {L"Icon:", kIconBrowse, kIconRemove, L"Studio's icon",
     L"Icons and pictures (*.ico, *.png)\0*.ico;*.png\0All pictures\0*.ico;*.png;*.bmp;*.gif;*.jpg;*.jpeg;*.tif;*.tiff\0",
     L"Pick a window icon", check_icon},
};

bool run_file_dialog(OPENFILENAMEW& dialog) {
  using DialogFn = BOOL(WINAPI*)(LPOPENFILENAMEW);
  static HMODULE comdlg = LoadLibraryW(L"comdlg32.dll");
  const auto run = comdlg ? reinterpret_cast<DialogFn>(GetProcAddress(comdlg, "GetOpenFileNameW")) : nullptr;
  return run && run(&dialog);
}

void browse_mod(size_t index) {
  const ModPicker& picker = kModPickers[index];
  std::wstring& value = g.pending_mods[index];
  wchar_t file[MAX_PATH] = {};
  if (!value.empty()) wcsncpy(file, value.c_str(), MAX_PATH - 1);
  OPENFILENAMEW dialog{};
  dialog.lStructSize = sizeof dialog;
  dialog.hwndOwner = g.dialog.hwnd;
  dialog.lpstrFilter = picker.filter;
  dialog.lpstrFile = file;
  dialog.nMaxFile = MAX_PATH;
  dialog.lpstrTitle = picker.title;
  dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_EXPLORER;
  if (!run_file_dialog(dialog)) return;
  g.mod_error[index] = picker.check(file);
  if (!g.mod_error[index].empty()) return;
  value = file;
  mark_dirty();
}

void open_mods_folder() {
  wchar_t folder[MAX_PATH];
  mods_folder(&g.paths, folder);
  SHCreateDirectoryExW(nullptr, (std::wstring(folder) + L"\\content").c_str(), nullptr);
  ShellExecuteW(g.dialog.hwnd, L"open", folder, nullptr, nullptr, SW_SHOWNORMAL);
}

void page_mods(const D2D1_RECT_F& page) {
  const float x = page.left + kMargin, w = page.right - page.left - kMargin * 2, ix = x + 9, iw = w - 18, label = 84;
  float y = page.top + kMargin;
  const auto file_row = [&](float row, size_t index) {
    const ModPicker& picker = kModPickers[index];
    const std::wstring& value = g.pending_mods[index];
    const std::wstring& error = g.mod_error[index];
    text(picker.label, g.fonts.body, rect(ix, row + 4, label, 15), kText);
    const D2D1_RECT_F box = rect(ix + label, row, iw - label - kButtonWidth * 2 - kGap * 2, kButtonHeight);
    fill_rect(box, ink(kWhite));
    stroke_rect(box, 0xABADB3);
    const size_t slash = value.find_last_of(L"\\/");
    const std::wstring shown = !error.empty() ? error : value.empty() ? picker.none : value.substr(slash == std::wstring::npos ? 0 : slash + 1);
    text(shown, g.fonts.field, D2D1::RectF(box.left + 5, box.top, box.right - 5, box.bottom),
         !error.empty() ? kError : value.empty() ? kDisabled : kText);
    push_button(picker.browse, rect(box.right + kGap, row, kButtonWidth, kButtonHeight), L"Browse...", true, [index] { browse_mod(index); });
    push_button(picker.remove, rect(box.right + kGap * 2 + kButtonWidth, row, kButtonWidth, kButtonHeight), L"Remove", !value.empty(),
                [index] {
                  g.pending_mods[index].clear();
                  g.mod_error[index].clear();
                  mark_dirty();
                });
  };

  group(rect(x, y, w, 100), L"Roblox Studio");
  file_row(y + 16, MOD_ARROW);
  file_row(y + 44, MOD_HAND);
  file_row(y + 72, MOD_FONT);
  y += 100 + kMargin;

  group(rect(x, y, w, 44), L"Window");
  file_row(y + 16, MOD_ICON);
  y += 44 + kMargin;

  group(rect(x, y, w, 72), L"Mods folder");
  text(L"Pictures, fonts and sounds in it replace Studio's own. It's laid out like Studio's folder, starting at content.",
       g.fonts.paragraph, rect(ix, y + 16, iw, 30), kSecondary);
  link(kModsLink, ix, y + 48, L"Open the mods folder", open_mods_folder);
}

void page_about(const D2D1_RECT_F& page) {
  const float x = page.left + kMargin, w = page.right - page.left - kMargin * 2;
  float y = page.top + kMargin;
  draw_logo(x + 24, y + 24, 36);
  text(L"Studio7", g.fonts.instruction, rect(x + 60, y, w - 60, 21), kInstruction);
  text(L"Version " + widen(S7_VERSION), g.fonts.body, rect(x + 60, y + 24, w - 60, 15), kSecondary);
  text(L"Roblox Studio ported back to Windows 7.", g.fonts.body, rect(x + 60, y + 39, w - 60, 15), kText);
  y += 66;
  text(L"Runs Roblox's own Studio on Windows 7 by giving it the Windows functions it looks for. Part of Blox7.",
       g.fonts.paragraph, rect(x, y, w, 32), kText);
  y += 40;
  text(L"Studio7 is not affiliated with or endorsed by Roblox Corporation or Microsoft. Roblox and the Roblox logo are "
       L"trademarks of Roblox Corporation. Windows is a trademark of Microsoft.",
       g.fonts.paragraph, rect(x, y, w, 46), kSecondary);
  y += 58;
  link(kFolderLink, x, y, L"Open the Studio7 folder", [] { ShellExecuteW(g.dialog.hwnd, L"open", g.paths.base, nullptr, nullptr, SW_SHOWNORMAL); });
  link(kLogLink, x + 158, y, L"Open the log", [] { ShellExecuteW(g.dialog.hwnd, L"open", g.paths.log, nullptr, nullptr, SW_SHOWNORMAL); });
  link(kLicensesLink, x + 256, y, L"View licenses", [] {
    wchar_t notices[MAX_PATH];
    if (notices_write(&g.paths, notices)) ShellExecuteW(g.dialog.hwnd, L"open", notices, nullptr, nullptr, SW_SHOWNORMAL);
  });
  y += 28;
  checkbox(kSelfBox, x, y, w, g.pending_self, L"Get new versions of Studio7 as they come out", [] {
    g.pending_self = !g.pending_self;
    mark_dirty();
  });

  const float before = text_width(L"Made with ", g.fonts.body), after = text_width(L" by Zeph", g.fonts.body);
  const float left = std::round(x + (w - before - 11 - after) / 2), cy = page.bottom - kMargin - 15;
  text(L"Made with ", g.fonts.body, rect(left, cy, before + 2, 15), kSecondary);
  heart(left + before + 5.5f, cy + 8);
  text(L" by Zeph", g.fonts.body, rect(left + before + 11, cy, after + 2, 15), kSecondary);
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
      clickable(id, r, [tab = kTabs[i].tab] { g.tab = tab; });
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

void close_settings();
void apply_settings();

void draw_dialog() {
  const float w = c->width, h = c->height;
  fill_rect(D2D1::RectF(0, 0, w, h), ink(kFace));
  const D2D1_RECT_F strip = D2D1::RectF(kMargin, kMargin, w - kMargin, kMargin + kTabHeight);
  const D2D1_RECT_F page = D2D1::RectF(kMargin, strip.bottom, w - kMargin, h - kMargin - kButtonHeight - kMargin);
  fill_rect(page, ink(kWhite));
  stroke_rect(page, kTabEdge);
  draw_tabs(strip);
  const D2D1_RECT_F inner = inset(page, 1);
  if (g.tab == Tab::General) page_general(inner);
  else if (g.tab == Tab::Mods) page_mods(inner);
  else page_about(inner);
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
  for (auto& entry : c->springs) entry.second.target = 0;
  c->rt->BeginDraw();
  c->rt->SetTransform(D2D1::Matrix3x2F::Identity());
  if (c->draw) c->draw();
  return c->rt->EndDraw();
}

void paint() {
  g.clock = static_cast<float>(GetTickCount64() % 3600000) / 1000.0f;
  if (!create_device(*c)) return;
  if (draw_frame() == D2DERR_RECREATE_TARGET) drop_device(*c);
  sync_timer();
}

void repaint(Canvas& canvas) {
  if (canvas.hwnd) InvalidateRect(canvas.hwnd, nullptr, FALSE);
}

void apply_settings() {
  if (!g.dirty) return;
  state_set(&g.paths, L"updates", g.pending_updates ? "" : "off");
  state_set(&g.paths, L"self", g.pending_self ? "" : "off");
  if (g.pending_links != (links_wanted(&g.paths) != FALSE) && !links_set(&g.paths, g.pending_links)) {
    MessageBoxW(g.dialog.hwnd, L"Studio7 couldn't change who opens Roblox Studio links.", L"Studio7", MB_ICONERROR);
    g.pending_links = links_wanted(&g.paths) != FALSE;
    return;
  }
  for (int i = 0; i < MOD_COUNT; ++i) {
    wchar_t now[MAX_PATH], icon[MAX_PATH];
    mods_get(&g.paths, i, now);
    if (g.pending_mods[i] == now) continue;
    if (i == MOD_ICON) {
      mods_icon(&g.paths, icon);
      if (g.pending_mods[i].empty()) DeleteFileW(icon);
      else g.mod_error[i] = save_icon(g.pending_mods[i], icon);
      if (!g.mod_error[i].empty()) {
        g.tab = Tab::Mods;
        repaint(g.dialog);
        return;
      }
    }
    mods_set(&g.paths, i, g.pending_mods[i].c_str());
  }
  g.dirty = false;
  refresh_info();
  if (g.automatic) g.updates.clear();
  repaint(g.main);
}

void restart_if_idle() {
  if (g.dialog.hwnd || g.working || (g.stage != Stage::Ready && g.stage != Stage::Fresh)) return;
  wchar_t self[MAX_PATH];
  const DWORD length = GetModuleFileNameW(nullptr, self, MAX_PATH);
  if (!length || length >= MAX_PATH) return;
  std::wstring command = L"\"" + std::wstring(self) + L"\"";
  STARTUPINFOW startup{};
  startup.cb = sizeof startup;
  PROCESS_INFORMATION process{};
  if (g.single) {
    ReleaseMutex(g.single);
    CloseHandle(g.single);
  }
  if (!CreateProcessW(self, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process)) {
    g.single = CreateMutexW(nullptr, TRUE, L"Local\\Studio7.Launcher");
    return;
  }
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  DestroyWindow(g.main.hwnd);
}

void close_settings() {
  if (!g.dialog.hwnd) return;
  EnableWindow(g.main.hwnd, TRUE);
  SetForegroundWindow(g.main.hwnd);
  DestroyWindow(g.dialog.hwnd);
}

void remove_all() {
  if (MessageBoxW(g.dialog.hwnd,
                  L"Remove Roblox Studio and Studio7's files from this account?\n\nYour places, plugins and Roblox Studio's own "
                  L"settings stay.",
                  L"Studio7", MB_YESNO | MB_ICONQUESTION) != IDYES)
    return;
  if (!studio_remove(&g.paths)) {
    MessageBoxW(g.dialog.hwnd, L"Roblox Studio is still open, or another Studio7 is busy. Close them and try again.", L"Studio7",
                MB_ICONWARNING);
    return;
  }
  MessageBoxW(g.dialog.hwnd, L"Roblox Studio was removed. Studio7.exe itself is yours to delete.", L"Studio7", MB_ICONINFORMATION);
  close_settings();
  DestroyWindow(g.main.hwnd);
}

void fail(bool start, const std::wstring& text) {
  char lines[8][200];
  const int count = log_tail(&g.paths, lines, 8);
  g.stage = Stage::Failed;
  g.failed_start = start;
  g.failure = text;
  g.error_lines.clear();
  for (int i = count - 1; i >= 0; --i) g.error_lines.push_back(widen(lines[i]));
  g.details = false;
}

void start_job(Job job, bool quiet) {
  if (g.working.exchange(true)) return;
  g.quiet = quiet;
  g.stop = false;
  g.fraction = -1;
  {
    std::lock_guard<std::mutex> hold(g.lock);
    g.job_text.clear();
  }
  const bool installed = !g.version.empty(), update = g.automatic || g.asked;
  g.asked = false;
  std::thread([job, installed, update] {
    auto* out = new Outcome;
    char error[400] = "";
    out->job = job;
    if (job == Job::Check) {
      out->answer = studio_check(&g.paths, &out->latest, !installed);
      if (out->answer == CHECK_NEWER && installed && update) {
        out->job = Job::Get;
        out->result = studio_get(&g.paths, &out->latest, FALSE, error, sizeof error);
      }
    } else {
      out->result = studio_get(&g.paths, nullptr, TRUE, error, sizeof error);
    }
    out->error = widen(error);
    if (!PostMessageW(g.main.hwnd, kJobDone, 0, reinterpret_cast<LPARAM>(out))) delete out;
  }).detach();
}

void open_studio() {
  if (!run_start(&g.paths, g.arguments.c_str(), &g.run)) {
    refresh_info();
    if (g.version.empty()) {
      g.stage = Stage::Fresh;
      g.detail = L"Roblox Studio's files are gone, so it has to be downloaded again.";
    } else {
      fail(true, start_failure(g.run));
    }
    fit_main_window();
    repaint(g.main);
    return;
  }
  g.arguments.clear();
  g.detail.clear();
  g.stage = Stage::Starting;
  g.fraction = -1;
  SetTimer(g.main.hwnd, kWatchTimer, 250, nullptr);
  fit_main_window();
  repaint(g.main);
}

void install() {
  g.detail.clear();
  g.stage = Stage::Setting;
  if (g.working) g.install_after = true;
  else start_job(Job::Get, false);
  fit_main_window();
  repaint(g.main);
}

void act() {
  switch (g.stage) {
    case Stage::Fresh: install(); break;
    case Stage::Setting:
      if (g.install_after) {
        g.install_after = false;
        g.stage = Stage::Fresh;
        fit_main_window();
      } else {
        g.stop = true;
      }
      repaint(g.main);
      break;
    case Stage::Ready: open_studio(); break;
    case Stage::Failed:
      if (g.failed_start && !g.version.empty()) open_studio();
      else install();
      break;
    default: break;
  }
}

void job_done(Outcome* out) {
  const bool quiet = g.quiet;
  g.working = false;
  g.stop = false;
  refresh_info();
  if (g.install_after) {
    g.install_after = false;
    delete out;
    start_job(Job::Get, false);
    return;
  }
  const std::wstring name = widen(out->latest.name);
  if (out->job == Job::Check) {
    if (out->answer == CHECK_FAILED) g.updates = L"Couldn't reach Roblox to check";
    else if (out->answer == CHECK_REFUSED) g.updates = name + L" doesn't work with this Studio7 yet";
    else if (out->answer == CHECK_CURRENT) g.updates = L"Up to date";
    else g.updates = g.version.empty() ? L"" : name + L" is out";
    g.newer = out->answer == CHECK_NEWER && !g.version.empty() ? name : L"";
    if (g.version.empty() && out->answer != CHECK_FAILED && out->latest.download)
      g.offer = L"Roblox Studio " + name + L", a " + megabytes(out->latest.download) + L" download.";
  } else if (quiet) {
    if (out->result == SETUP_OK) g.updates = L"Updated to " + name;
    else if (out->result == SETUP_STOPPED) g.updates.clear();
    else g.updates = out->error;
    if (out->result == SETUP_OK) g.newer.clear();
    if (out->result == SETUP_OK && !g.version.empty() && g.stage == Stage::Fresh) g.stage = Stage::Ready;
  } else if (out->result == SETUP_OK && !g.version.empty()) {
    g.stage = Stage::Ready;
    g.updates = L"Up to date";
    g.detail.clear();
    links_refresh(&g.paths);
    if (g.open_when_ready) {
      g.open_when_ready = false;
      open_studio();
    }
  } else if (out->result == SETUP_STOPPED) {
    g.stage = g.version.empty() ? Stage::Fresh : Stage::Ready;
    g.detail = L"Stopped. What was downloaded is kept for next time.";
  } else {
    fail(false, out->error.empty() ? L"Roblox Studio could not be set up." : out->error);
  }
  delete out;
  fit_main_window();
  repaint(g.main);
  repaint(g.dialog);
}

void watch_studio() {
  const std::wstring failed = widen(g.run.name);
  const int state = run_poll(&g.paths, &g.run);
  if (state == RUN_STARTING) return;
  if (state == RUN_SHOWN) {
    if (g.stage != Stage::Starting) return;
    g.stage = Stage::Running;
    if (!IsIconic(g.main.hwnd)) {
      ShowWindow(g.main.hwnd, SW_MINIMIZE);
      g.minimized_by_us = true;
    }
    fit_main_window();
    repaint(g.main);
    return;
  }
  KillTimer(g.main.hwnd, kWatchTimer);
  refresh_info();
  if (state == RUN_AGAIN) {
    g.updates = failed + L" didn't start here, went back";
    open_studio();
    return;
  }
  if (state == RUN_FAILED) {
    fail(true, start_failure(g.run));
  } else {
    g.stage = Stage::Ready;
    g.detail = L"Roblox Studio closed. See you next time.";
  }
  if (g.minimized_by_us) {
    ShowWindow(g.main.hwnd, SW_RESTORE);
    SetForegroundWindow(g.main.hwnd);
    g.minimized_by_us = false;
  }
  fit_main_window();
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
      return 0;
    case WM_MOUSELEAVE:
      c->tracking = false;
      c->hot = 0;
      InvalidateRect(hwnd, nullptr, FALSE);
      return 0;
    case WM_LBUTTONDOWN: {
      track_mouse(lp);
      const Hit* hit = hit_at(c->mouse_x, c->mouse_y);
      c->pressed = hit ? hit->id : 0;
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
        const Hit* hit = hit_at(c->mouse_x, c->mouse_y);
        if (hit && hit->id == pressed && hit->action) {
          const std::function<void()> action = hit->action;
          action();
        }
        if (IsWindow(hwnd)) InvalidateRect(hwnd, nullptr, FALSE);
      }
      return 0;
    case WM_SETCURSOR:
      if (LOWORD(lp) == HTCLIENT) {
        const Hit* hit = hit_at(c->mouse_x, c->mouse_y);
        SetCursor(LoadCursorW(nullptr, hit && hit->cursor == Cursor::Hand ? IDC_HAND : IDC_ARROW));
        return TRUE;
      }
      break;
    case WM_KEYDOWN:
      if (wp == VK_ESCAPE && c == &g.dialog) {
        close_settings();
      } else if (wp == VK_RETURN) {
        if (c == &g.main && g.stage != Stage::Setting) act();
        else if (c == &g.dialog) {
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
      else if (wp == kWatchTimer) watch_studio();
      return 0;
    case kProgress:
      if (g.stage == Stage::Setting) fit_main_window();
      InvalidateRect(hwnd, nullptr, FALSE);
      repaint(g.dialog);
      return 0;
    case kJobDone:
      job_done(reinterpret_cast<Outcome*>(lp));
      return 0;
    case kSelfUpdated:
      restart_if_idle();
      return 0;
    case WM_CLOSE:
      if (c == &g.dialog) {
        close_settings();
        return 0;
      }
      g.stop = true;
      break;
    case WM_DESTROY:
      drop_device(*c);
      KillTimer(hwnd, kAnimTimer);
      c->timer_on = false;
      c->hwnd = nullptr;
      c->hits.clear();
      c->springs.clear();
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
  g.tab = tab;
  if (g.dialog.hwnd) {
    SetForegroundWindow(g.dialog.hwnd);
    repaint(g.dialog);
    return;
  }
  g.pending_updates = g.automatic;
  g.pending_links = links_wanted(&g.paths) != FALSE;
  g.pending_self = self_updates(g.paths);
  for (int i = 0; i < MOD_COUNT; ++i) {
    wchar_t file[MAX_PATH];
    mods_get(&g.paths, i, file);
    g.pending_mods[i] = file;
    g.mod_error[i].clear();
  }
  g.dirty = false;
  g.dialog.draw = draw_dialog;
  HWND hwnd = create_window(g.dialog, kDialogClass, L"Studio7 Settings", WS_POPUP | WS_CAPTION | WS_SYSMENU, WS_EX_DLGMODALFRAME,
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
  CreateDirectoryW(dir.c_str(), nullptr);
  g.main.draw = draw_main;
  g.dialog.draw = draw_dialog;
  int written = 0;
  struct MainShot {
    const wchar_t* name;
    Stage stage;
    int hot;
    const wchar_t *version, *updates, *text, *webview;
    double fraction;
    bool details;
  } mains[] = {
      {L"main-fresh", Stage::Fresh, kOpenButton, L"", L"", L"Roblox Studio 0.748.0.7481526, a 570 MB download.", L"109.0.1518.140", -1, false},
      {L"main-setting", Stage::Setting, 0, L"", L"", L"Downloading, 312 of 570 MB (5.7 MB/s)", L"109.0.1518.140", 0.49, false},
      {L"main-ready", Stage::Ready, kOpenButton, L"0.748.0.7481526", L"Up to date", L"", L"109.0.1518.140", -1, false},
      {L"main-updating", Stage::Ready, 0, L"0.748.0.7481526", L"Downloading, 96 of 571 MB (5.4 MB/s)", L"", L"109.0.1518.140", -1, false},
      {L"main-starting", Stage::Starting, 0, L"0.748.0.7481526", L"Up to date", L"", L"109.0.1518.140", -1, false},
      {L"main-running", Stage::Running, 0, L"0.748.0.7481526", L"Up to date", L"", L"109.0.1518.140", -1, false},
      {L"main-no-webview", Stage::Ready, kWebviewLink, L"0.748.0.7481526", L"Up to date", L"", L"", -1, false},
      {L"main-failed", Stage::Failed, kDetailsButton, L"", L"", L"setup.rbxcdn.com did not answer (error 10060)", L"109.0.1518.140", -1, true},
      {L"main-unsupported", Stage::Unsupported, 0, L"", L"", L"This Windows runs Roblox Studio from roblox.com as it is.",
       L"140.0.3485.54", -1, false}};
  struct DialogShot {
    const wchar_t* name;
    Tab tab;
    int hot;
  } dialogs[] = {{L"settings-general", Tab::General, kCheckButton},
                 {L"settings-mods", Tab::Mods, kFontBrowse},
                 {L"settings-about", Tab::About, kFolderLink}};
  for (const float scale : {1.0f, 1.5f}) {
    for (const MainShot& shot : mains) {
      g.stage = shot.stage;
      g.version = shot.version;
      g.updates = shot.updates;
      g.webview = shot.webview;
      g.detail = shot.stage == Stage::Unsupported ? shot.text : L"";
      g.offer = shot.stage == Stage::Fresh ? shot.text : L"";
      g.failure = shot.stage == Stage::Failed ? shot.text : L"";
      g.job_text = shot.stage == Stage::Setting ? shot.text : L"";
      g.fraction = shot.fraction;
      g.details = shot.details;
      g.error_lines = {L"getting Studio 0.748.0.7481526 (version-8d7a1009c17e4ff8)",
                       L"RobloxStudio.zip: cannot connect to setup.rbxcdn.com (error 10060)",
                       L"setup failed: RobloxStudio.zip: cannot connect to setup.rbxcdn.com (error 10060)"};
      g.clock = 0.9f;
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
    g.version = L"0.748.0.7481526";
    g.updates = L"Up to date";
    g.pending_updates = g.pending_links = true;
    g.pending_mods[MOD_ARROW] = L"C:\\Mods\\2013\\ArrowFarCursor.png";
    g.pending_mods[MOD_HAND] = L"C:\\Mods\\2013\\ArrowCursor.png";
    g.pending_mods[MOD_ICON] = L"C:\\Mods\\2013\\studio-2013.ico";
    for (const DialogShot& shot : dialogs) {
      g.tab = shot.tab;
      g.dialog.springs.clear();
      g.dialog.hot = shot.hot;
      g.dialog.active = true;
      g.dirty = false;
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

int run_launcher(HINSTANCE instance, int show, const Paths& paths, const std::wstring& arguments, bool open_when_ready,
                 const std::wstring& failure) {
  HANDLE single = g.single = CreateMutexW(nullptr, TRUE, L"Local\\Studio7.Launcher");
  if (single && GetLastError() == ERROR_ALREADY_EXISTS) {
    if (HWND other = FindWindowW(kMainClass, nullptr)) {
      ShowWindow(other, SW_RESTORE);
      SetForegroundWindow(other);
    }
    return 0;
  }
  if (!init_graphics()) {
    MessageBoxW(nullptr, L"Studio7 needs Direct2D (Windows 7 SP1 or newer).", L"Studio7", MB_ICONERROR);
    return 1;
  }
  g.paths = paths;
  g.arguments = arguments;
  g.open_when_ready = open_when_ready;
  refresh_info();
  if (!windows_seven()) {
    DWORD major = 0, minor = 0, build = 0;
    windows_version(&major, &minor, &build);
    g.stage = Stage::Unsupported;
    g.detail = major >= 10 ? L"This Windows runs Roblox Studio from roblox.com as it is." : L"Windows 8 and 8.1 aren't supported yet.";
  } else if (!failure.empty()) {
    fail(true, failure);
  } else if (!g.version.empty()) {
    g.stage = Stage::Ready;
    links_refresh(&g.paths);
  }

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
  HWND hwnd = create_window(g.main, kMainClass, L"Studio7", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, 0, kMainWidth,
                            main_content_height() + kCommandHeight, nullptr);
  if (!hwnd) return 1;
  if (HMENU menu = GetSystemMenu(hwnd, FALSE)) {
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kAboutCommand, L"About Studio7...");
  }
  ShowWindow(hwnd, show == SW_HIDE ? SW_SHOWNORMAL : show);
  UpdateWindow(hwnd);

  if (g.stage == Stage::Fresh && g.open_when_ready) {
    install();
  } else if (g.stage == Stage::Fresh || (g.stage == Stage::Ready && g.automatic)) {
    if (g.stage == Stage::Ready) g.updates = L"Checking...";
    start_job(Job::Check, true);
  }
  std::thread([hwnd] {
    if (update_self(g.paths)) PostMessageW(hwnd, kSelfUpdated, 0, 0);
  }).detach();

  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  return static_cast<int>(msg.wParam);
}

}  // namespace studio7

void ui_status(const char* format, ...) {
  char text[300];
  va_list args;
  va_start(args, format);
  _vsnprintf(text, sizeof text - 1, format, args);
  va_end(args);
  text[sizeof text - 1] = 0;
  {
    std::lock_guard<std::mutex> hold(studio7::g.lock);
    studio7::g.job_text = studio7::widen(text);
  }
  if (studio7::g.main.hwnd) PostMessageW(studio7::g.main.hwnd, studio7::kProgress, 0, 0);
}

void ui_progress(double fraction) {
  studio7::g.fraction = fraction;
  if (studio7::g.main.hwnd) PostMessageW(studio7::g.main.hwnd, studio7::kProgress, 0, 0);
}

BOOL ui_cancelled(void) { return studio7::g.stop.load(); }

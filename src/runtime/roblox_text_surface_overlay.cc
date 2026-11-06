#include "runtime/roblox_text_surface_overlay.h"

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>
#ifndef BLOX7_WIN32
#include <fontconfig/fontconfig.h>
#else
#include <unistd.h>
#endif

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "runtime/roblox_text_font_resolver.h"

namespace blox7 {
namespace runtime {
namespace {

constexpr std::size_t kMaximumRasterTextBytes = 4096;
constexpr std::uint64_t kMaximumOverlayPixels = 16ULL * 1024ULL * 1024ULL;
constexpr std::size_t kMaximumFallbackFonts = 6;

std::mutex g_active_overlay_mutex;
RobloxTextSurfaceOverlay* g_active_overlay = nullptr;
std::atomic<bool> g_overlay_may_present{false};

void SecureClear(std::string* value) {
  if (value == nullptr) {
    return;
  }
  volatile char* bytes = value->empty() ? nullptr : value->data();
  for (std::size_t index = 0; index < value->size(); ++index) {
    bytes[index] = 0;
  }
  value->clear();
}

void SecureClear(std::vector<std::uint8_t>* value) {
  if (value == nullptr) {
    return;
  }
  volatile std::uint8_t* bytes = value->empty() ? nullptr : value->data();
  for (std::size_t index = 0; index < value->size(); ++index) {
    bytes[index] = 0;
  }
  value->clear();
}

struct SensitiveString {
  ~SensitiveString() { SecureClear(&value); }
  std::string value;
};

bool IsUtf8Continuation(unsigned char value) {
  return (value & 0xC0U) == 0x80U;
}

SensitiveString VisibleTextWindow(
    const RobloxTextOverlayPresentation& presentation, std::size_t* caret_byte,
    std::size_t* selection_begin_byte, std::size_t* selection_end_byte,
    std::size_t* window_begin_byte) {
  SensitiveString result;
  *window_begin_byte = 0;
  const std::string& text = presentation.display_utf8;
  const std::size_t caret = std::min(presentation.caret_utf8_byte, text.size());
  const std::size_t selection_begin =
      std::min(presentation.selection_begin_utf8_byte, text.size());
  const std::size_t selection_end =
      std::min(presentation.selection_end_utf8_byte, text.size());
  if (text.size() <= kMaximumRasterTextBytes) {
    result.value = text;
    *caret_byte = caret;
    *selection_begin_byte = std::min(selection_begin, selection_end);
    *selection_end_byte = std::max(selection_begin, selection_end);
    return result;
  }

  const std::size_t half = kMaximumRasterTextBytes / 2;
  std::size_t begin = caret > half ? caret - half : 0;
  std::size_t end = std::min(text.size(), begin + kMaximumRasterTextBytes);
  if (end - begin < kMaximumRasterTextBytes && begin != 0) {
    begin = end - kMaximumRasterTextBytes;
  }
  while (begin < text.size() &&
         IsUtf8Continuation(static_cast<unsigned char>(text[begin]))) {
    ++begin;
  }
  while (end > begin && end < text.size() &&
         IsUtf8Continuation(static_cast<unsigned char>(text[end]))) {
    --end;
  }
  result.value.assign(text.data() + begin, end - begin);
  *window_begin_byte = begin;
  *caret_byte = std::min(caret - begin, result.value.size());
  const auto map_to_window = [begin, end](std::size_t byte) {
    if (byte <= begin) {
      return std::size_t{0};
    }
    if (byte >= end) {
      return end - begin;
    }
    return byte - begin;
  };
  *selection_begin_byte =
      map_to_window(std::min(selection_begin, selection_end));
  *selection_end_byte = map_to_window(std::max(selection_begin, selection_end));
  return result;
}

std::vector<std::string> ResolveFontFiles(
    const RobloxTextFontSelection& selection) {
  std::vector<std::string> files;
  const char* configured = std::getenv("BLOX7_TEXT_FONT");
  if (configured != nullptr && configured[0] != '\0') {
    files.emplace_back(configured);
  } else if (!selection.primary_file.empty()) {
    files.push_back(selection.primary_file);
  }
#ifdef BLOX7_WIN32
  static const char* const kWindowsFonts[] = {
      "/mnt/c/Windows/Fonts/segoeui.ttf", "/mnt/c/Windows/Fonts/arial.ttf",
      "/mnt/c/Windows/Fonts/msyh.ttf",    "/mnt/c/Windows/Fonts/meiryo.ttc",
      "/mnt/c/Windows/Fonts/malgun.ttf",  "/mnt/c/Windows/Fonts/seguisym.ttf",
  };
  for (const char* font : kWindowsFonts) {
    if (files.size() >= kMaximumFallbackFonts) break;
    if (::access(font, R_OK) == 0 && std::find(files.begin(), files.end(), font) == files.end()) {
      files.emplace_back(font);
    }
  }
  return files;
#else
  if (!FcInit()) {
    return files;
  }

  FcPattern* pattern = FcNameParse(reinterpret_cast<const FcChar8*>("sans"));
  if (pattern == nullptr) {
    return files;
  }
  FcConfigSubstitute(nullptr, pattern, FcMatchPattern);
  FcDefaultSubstitute(pattern);
  FcResult result = FcResultNoMatch;
  FcFontSet* fonts = FcFontSort(nullptr, pattern, FcTrue, nullptr, &result);
  FcPatternDestroy(pattern);
  if (fonts == nullptr) {
    return files;
  }
  for (int index = 0;
       index < fonts->nfont && files.size() < kMaximumFallbackFonts; ++index) {
    FcChar8* file = nullptr;
    if (FcPatternGetString(fonts->fonts[index], FC_FILE, 0, &file) !=
            FcResultMatch ||
        file == nullptr || file[0] == '\0') {
      continue;
    }
    const std::string candidate(reinterpret_cast<const char*>(file));
    if (std::find(files.begin(), files.end(), candidate) == files.end()) {
      files.push_back(candidate);
    }
  }
  FcFontSetDestroy(fonts);
  return files;
#endif
}

void SetPixel(std::vector<std::uint8_t>* pixels, int width, int x, int y,
              std::uint8_t red, std::uint8_t green, std::uint8_t blue,
              std::uint8_t alpha) {
  if (pixels == nullptr || x < 0 || y < 0 || x >= width) {
    return;
  }
  const std::size_t offset =
      (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
       static_cast<std::size_t>(x)) *
      4;
  if (offset + 3 >= pixels->size()) {
    return;
  }
  (*pixels)[offset] = red;
  (*pixels)[offset + 1] = green;
  (*pixels)[offset + 2] = blue;
  (*pixels)[offset + 3] = alpha;
}

void BlendSurface(const SDL_Surface* surface, int destination_x,
                  int destination_y, int clip_left, int clip_top,
                  int clip_right, int clip_bottom, int destination_width,
                  std::uint8_t opacity,
                  std::vector<std::uint8_t>* destination) {
  if (surface == nullptr || surface->pixels == nullptr ||
      destination == nullptr) {
    return;
  }
  const auto* source = static_cast<const std::uint8_t*>(surface->pixels);
  for (int source_y = 0; source_y < surface->h; ++source_y) {
    const int y = destination_y + source_y;
    if (y < clip_top || y >= clip_bottom) {
      continue;
    }
    for (int source_x = 0; source_x < surface->w; ++source_x) {
      const int x = destination_x + source_x;
      if (x < clip_left || x >= clip_right) {
        continue;
      }
      const std::size_t source_offset =
          static_cast<std::size_t>(source_y) *
              static_cast<std::size_t>(surface->pitch) +
          static_cast<std::size_t>(source_x) * 4;
      const std::size_t destination_offset =
          (static_cast<std::size_t>(y) *
               static_cast<std::size_t>(destination_width) +
           static_cast<std::size_t>(x)) *
          4;
      const std::uint32_t coverage = source[source_offset + 3];
      const std::uint32_t alpha = (coverage * opacity + 127U) / 255U;
      if (alpha == 0) {
        continue;
      }
      for (std::size_t channel = 0; channel < 3; ++channel) {
        (*destination)[destination_offset + channel] =
            static_cast<std::uint8_t>(std::min(
                255U, (source[source_offset + channel] * 255U + coverage / 2U) /
                          coverage));
      }
      (*destination)[destination_offset + 3] = static_cast<std::uint8_t>(alpha);
    }
  }
}

Status RasterizationFailure(const char* operation) {
  std::string message = operation != nullptr ? operation : "text rasterization";
  const char* detail = SDL_GetError();
  if (detail != nullptr && detail[0] != '\0') {
    message.append(": ").append(detail);
  }
  return Status::Error(StatusCode::kPlatformError, std::move(message));
}

SDL_Color ResolveTextColor(std::int32_t packed_color) {
  const std::uint32_t packed = static_cast<std::uint32_t>(packed_color);
  const std::uint8_t alpha = static_cast<std::uint8_t>(packed >> 24U);
  if (alpha == 0) {
    return {212, 216, 225, 255};
  }
  return {static_cast<std::uint8_t>(packed >> 16U),
          static_cast<std::uint8_t>(packed >> 8U),
          static_cast<std::uint8_t>(packed), alpha};
}

#ifdef BLOX7_WIN32
// windows shows this 1:1 on its own layer, so draw at display pixels or it's blurry
float RasterScale(const RobloxTextOverlayViewport& viewport) {
  return viewport.density_scale;
}
#else
float RasterScale(const RobloxTextOverlayViewport&) { return 1.0F; }
#endif

int Scaled(float value, float scale) {
  return static_cast<int>(std::lround(value * scale));
}

std::size_t CodePointsBefore(const std::string& text, std::size_t byte) {
  std::size_t count = 0;
  for (std::size_t index = 0; index < byte && index < text.size(); ++index) {
    if (!IsUtf8Continuation(static_cast<unsigned char>(text[index]))) {
      ++count;
    }
  }
  return count;
}

struct TextBytePosition {
  int x = 0;
  int y = 0;
  int height = 0;
};

TextBytePosition TextPosition(TTF_Text* layout, std::size_t byte_offset,
                              int text_width, bool single_line) {
  if (layout == nullptr) {
    return {};
  }
  TTF_SubString substring{};
  const int offset = static_cast<int>(std::min<std::size_t>(
      byte_offset, static_cast<std::size_t>(std::numeric_limits<int>::max())));
  if (!TTF_GetTextSubString(layout, offset, &substring)) {
    return {};
  }
  TextBytePosition position;
  position.x = substring.rect.x;
  position.y = substring.rect.y;
  position.height = substring.rect.h;
  if ((substring.flags & TTF_SUBSTRING_TEXT_END) != 0) {
    position.x = single_line ? text_width
                             : std::min(text_width,
                                        substring.rect.x + substring.rect.w);
  }
  return position;
}

}  // namespace

struct RobloxTextSurfaceOverlay::RasterCache {
  ~RasterCache() { Close(); }

  bool OpenFonts(const std::vector<std::string>& wanted, float size) {
    if (!fonts.empty() && wanted == files && size == point_size) {
      return true;
    }
    CloseFonts();
    if (!ttf_ready) {
      if (!TTF_Init()) {
        return false;
      }
      ttf_ready = true;
    }
    for (const std::string& file : wanted) {
      TTF_Font* font = TTF_OpenFont(file.c_str(), size);
      if (font != nullptr) {
        fonts.push_back(font);
      }
    }
    if (fonts.empty()) {
      return false;
    }
    for (std::size_t index = 1; index < fonts.size(); ++index) {
      (void)TTF_AddFallbackFont(fonts.front(), fonts[index]);
    }
    files = wanted;
    point_size = size;
    return true;
  }

  void CloseLayout() {
    if (layout != nullptr) {
      TTF_DestroyText(layout);
      layout = nullptr;
    }
    window_bytes = 0;
  }

  void CloseFonts() {
    CloseLayout();
    if (!fonts.empty()) {
      TTF_ClearFallbackFonts(fonts.front());
      for (auto font = fonts.rbegin(); font != fonts.rend(); ++font) {
        TTF_CloseFont(*font);
      }
      fonts.clear();
    }
    files.clear();
    point_size = 0.0F;
  }

  void Close() {
    CloseFonts();
    if (engine != nullptr) {
      TTF_DestroySurfaceTextEngine(engine);
      engine = nullptr;
    }
    if (ttf_ready) {
      TTF_Quit();
      ttf_ready = false;
    }
  }

  bool ttf_ready = false;
  std::vector<std::string> files;
  float point_size = 0.0F;
  std::vector<TTF_Font*> fonts;
  TTF_TextEngine* engine = nullptr;
  TTF_Text* layout = nullptr;
  int text_offset = 0;
  int text_y = 0;
  float scale = 1.0F;
  std::size_t window_begin = 0;
  std::size_t window_bytes = 0;
};

RobloxTextSurfaceOverlay::RobloxTextSurfaceOverlay() = default;
RobloxTextSurfaceOverlay::~RobloxTextSurfaceOverlay() { (void)Shutdown(); }

Status RobloxTextSurfaceOverlay::Initialize(
    RobloxTextOverlayViewport viewport) {
  if (!viewport.valid()) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "text surface overlay viewport is invalid");
  }
  std::lock_guard<std::mutex> active_lock(g_active_overlay_mutex);
  std::lock_guard<std::mutex> lock(mutex_);
  if (initialized_ || g_active_overlay != nullptr) {
    return Status::Error(StatusCode::kFailedPrecondition,
                         "text surface overlay is already initialized");
  }
  viewport_ = viewport;
  failure_ = Status::Ok();
  initialized_ = true;
  g_active_overlay = this;
  g_overlay_may_present.store(false, std::memory_order_release);
  return Status::Ok();
}

Status RobloxTextSurfaceOverlay::Shutdown() {
  std::lock_guard<std::mutex> active_lock(g_active_overlay_mutex);
  std::lock_guard<std::mutex> lock(mutex_);
  const bool owned_active_overlay = g_active_overlay == this;
  if (owned_active_overlay) {
    g_active_overlay = nullptr;
  }
  state_.Reset();
  ClearFrameLocked();
  raster_cache_.reset();
  viewport_ = {};
  initialized_ = false;
  if (owned_active_overlay) {
    g_overlay_may_present.store(false, std::memory_order_release);
  }
  return failure_;
}

Status RobloxTextSurfaceOverlay::UpdateViewport(
    RobloxTextOverlayViewport viewport) {
  if (!viewport.valid()) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "text surface overlay viewport is invalid");
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (!initialized_) {
    return Status::Error(StatusCode::kFailedPrecondition,
                         "text surface overlay is not initialized");
  }
  viewport_ = viewport;
  return Status::Ok();
}

RobloxTextDisplaySink RobloxTextSurfaceOverlay::sink() {
  return {this, &RobloxTextSurfaceOverlay::UpdateCallback,
          &RobloxTextSurfaceOverlay::HitTestCallback};
}

bool RobloxTextSurfaceOverlay::HitTestCallback(void* context, float x, float y,
                                               bool clamp,
                                               std::size_t* code_points) {
  return context != nullptr &&
         static_cast<RobloxTextSurfaceOverlay*>(context)->HitTest(
             x, y, clamp, code_points);
}

bool RobloxTextSurfaceOverlay::HitTest(float x, float y, bool clamp,
                                       std::size_t* code_points) {
  if (code_points == nullptr) {
    return false;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (!initialized_) {
    return false;
  }
  const RobloxTextOverlayPresentation& presentation = state_.presentation();
  const RobloxTextOverlayGeometry& box = presentation.geometry;
  if (!presentation.visible || !box.valid()) {
    return false;
  }
  float local_x = x - static_cast<float>(box.x);
  float local_y = y - static_cast<float>(box.y);
  const bool inside = local_x >= 0.0F && local_y >= 0.0F &&
                      local_x < static_cast<float>(box.width) &&
                      local_y < static_cast<float>(box.height);
  if (!inside && !clamp) {
    return false;
  }
  local_x = std::clamp(local_x, 0.0F, static_cast<float>(box.width));
  local_y = std::clamp(local_y, 0.0F, static_cast<float>(box.height - 1));
  if (raster_revision_ != state_revision_) {
    Status status = RasterizeLocked();
    if (!status.ok()) {
      RecordFailureLocked(std::move(status));
      return false;
    }
  }
  const std::string& text = presentation.display_utf8;
  std::size_t byte = 0;
  if (raster_cache_ != nullptr) {
    const RasterCache& cache = *raster_cache_;
    if (cache.layout != nullptr && cache.window_bytes != 0) {
      const int point_x = Scaled(local_x, cache.scale) - cache.text_offset;
      const int point_y = Scaled(local_y, cache.scale) - cache.text_y;
      TTF_SubString substring{};
      if (TTF_GetTextSubStringForPoint(cache.layout, point_x, point_y,
                                       &substring)) {
        byte = static_cast<std::size_t>(std::max(0, substring.offset));
        if (point_x > substring.rect.x + substring.rect.w / 2) {
          byte += static_cast<std::size_t>(std::max(0, substring.length));
        }
        byte = std::min(byte, cache.window_bytes);
      }
    }
    byte += cache.window_begin;
  }
  *code_points = CodePointsBefore(text, std::min(byte, text.size()));
  return true;
}

void RobloxTextSurfaceOverlay::UpdateCallback(
    void* context, const RobloxTextDisplayUpdate& update) {
  if (context != nullptr) {
    static_cast<RobloxTextSurfaceOverlay*>(context)->ApplyUpdate(update);
  }
}

void RobloxTextSurfaceOverlay::ApplyUpdate(
    const RobloxTextDisplayUpdate& update) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!initialized_) {
    return;
  }
  bool changed = false;
  Status status = state_.Apply(update, viewport_, &changed);
  if (!status.ok()) {
    RecordFailureLocked(std::move(status));
    return;
  }
  if (!changed) {
    return;
  }
  ++state_revision_;
  if (state_revision_ == 0) {
    ++state_revision_;
  }
  const RobloxTextOverlayPresentation& presentation = state_.presentation();
  const bool drawable = presentation.visible && presentation.geometry.valid();
  if (!drawable) {
    ClearFrameLocked();
  }
  g_overlay_may_present.store(drawable, std::memory_order_release);
}

std::uint64_t RobloxTextSurfaceOverlay::revision() {
  std::lock_guard<std::mutex> lock(mutex_);
  return state_revision_;
}

bool RobloxTextSurfaceOverlay::QueryFrame(Blox7TextOverlayFrameInfo* frame) {
  if (frame == nullptr) {
    return false;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  *frame = {};
  frame->revision = state_revision_;
  if (!initialized_) {
    return false;
  }
  const RobloxTextOverlayPresentation& presentation = state_.presentation();
  if (!presentation.visible) {
    return true;
  }
  if (!presentation.geometry.valid()) {
    return true;
  }
  if (raster_revision_ != state_revision_) {
    Status status = RasterizeLocked();
    if (!status.ok()) {
      RecordFailureLocked(std::move(status));
      return false;
    }
  }
  if (rgba_.empty() || !presentation.geometry.valid()) {
    return false;
  }
  frame->visible = 1;
  frame->coordinate_width =
      static_cast<std::uint32_t>(viewport_.coordinate_width());
  frame->coordinate_height =
      static_cast<std::uint32_t>(viewport_.coordinate_height());
  frame->x = presentation.geometry.x;
  frame->y = presentation.geometry.y;
  frame->width = static_cast<std::uint32_t>(presentation.geometry.width);
  frame->height = static_cast<std::uint32_t>(presentation.geometry.height);
  frame->pixel_width = static_cast<std::uint32_t>(raster_width_);
  frame->pixel_height = static_cast<std::uint32_t>(raster_height_);
  frame->row_bytes = frame->pixel_width * 4;
  frame->rgba_bytes = rgba_.size();
  return true;
}

bool RobloxTextSurfaceOverlay::CopyFrame(std::uint64_t revision, void* rgba,
                                         std::size_t rgba_capacity) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!initialized_ || revision == 0 || revision != raster_revision_ ||
      rgba == nullptr || rgba_capacity < rgba_.size() || rgba_.empty()) {
    return false;
  }
  std::memcpy(rgba, rgba_.data(), rgba_.size());
  return true;
}

Status RobloxTextSurfaceOverlay::RasterizeLocked() {
  const RobloxTextOverlayPresentation& presentation = state_.presentation();
  if (!presentation.visible || !presentation.geometry.valid()) {
    return Status::Error(StatusCode::kFailedPrecondition,
                         "visible text surface geometry is unavailable");
  }
  const float scale = RasterScale(viewport_);
  const std::uint64_t width = static_cast<std::uint64_t>(
      std::max(1, Scaled(static_cast<float>(presentation.geometry.width), scale)));
  const std::uint64_t height = static_cast<std::uint64_t>(
      std::max(1, Scaled(static_cast<float>(presentation.geometry.height), scale)));
  if (width * height > kMaximumOverlayPixels ||
      width * height > std::numeric_limits<std::size_t>::max() / 4) {
    return Status::Error(StatusCode::kUnsupported,
                         "text surface dimensions exceed the safety limit");
  }

  std::vector<std::uint8_t> candidate(
      static_cast<std::size_t>(width * height * 4), 0);
  const int frame_width = static_cast<int>(width);
  const int frame_height = static_cast<int>(height);
  const int clip_left = 0;
  const int clip_right = frame_width;
  const int clip_top = 0;
  const int clip_bottom = frame_height;
  const int available_width = std::max(1, clip_right - clip_left);
  const bool wrapped_layout =
      presentation.multiline || presentation.text_wrapped;

  if (raster_cache_ == nullptr) {
    raster_cache_ = std::make_unique<RasterCache>();
  }
  RasterCache& cache = *raster_cache_;
  cache.CloseLayout();
  const RobloxTextFontSelection font_selection =
      ResolveRobloxTextFont(presentation.font);
  const float native_point_size =
      std::isfinite(presentation.font_size) ? presentation.font_size : 0.0F;
  const float point_size =
      native_point_size > 0.0F
          ? std::clamp(native_point_size * font_selection.size_scale * scale,
                       1.0F, 512.0F * scale)
          : std::clamp(static_cast<float>(height) * 0.38F, 12.0F * scale,
                       28.0F * scale);
  if (!cache.OpenFonts(ResolveFontFiles(font_selection), point_size)) {
    SecureClear(&candidate);
    return RasterizationFailure("unable to open a Roblox or fallback font");
  }
  if (cache.engine == nullptr) {
    cache.engine = TTF_CreateSurfaceTextEngine();
  }
  TTF_Font* const font = cache.fonts.front();
  if (wrapped_layout) {
    TTF_HorizontalAlignment alignment = TTF_HORIZONTAL_ALIGN_LEFT;
    if (presentation.x_alignment == 1) {
      alignment = TTF_HORIZONTAL_ALIGN_RIGHT;
    } else if (presentation.x_alignment == 2) {
      alignment = TTF_HORIZONTAL_ALIGN_CENTER;
    }
    TTF_SetFontWrapAlignment(font, alignment);
  }

  std::size_t caret_byte = 0;
  std::size_t selection_begin_byte = 0;
  std::size_t selection_end_byte = 0;
  std::size_t window_begin_byte = 0;
  SensitiveString text =
      VisibleTextWindow(presentation, &caret_byte, &selection_begin_byte,
                        &selection_end_byte, &window_begin_byte);
  SDL_Surface* rendered = nullptr;
  const SDL_Color text_color = ResolveTextColor(presentation.text_color);
  TTF_Text* layout =
      cache.engine != nullptr
          ? TTF_CreateText(cache.engine, font, text.value.data(),
                           text.value.size())
          : nullptr;
  bool success = layout != nullptr;
  if (success && wrapped_layout) {
    success = TTF_SetTextWrapWidth(layout, available_width) &&
              TTF_SetTextWrapWhitespaceVisible(layout, true);
  }
  int text_width = 0;
  int text_height = std::max(1, TTF_GetFontHeight(font));
  TextBytePosition caret_position;
  TextBytePosition selection_begin_position;
  TextBytePosition selection_end_position;
  if (success) {
    success = TTF_GetTextSize(layout, &text_width, &text_height);
    text_height = std::max(text_height, TTF_GetFontHeight(font));
  }
  if (success) {
    caret_position =
        TextPosition(layout, caret_byte, text_width, !wrapped_layout);
    selection_begin_position = TextPosition(layout, selection_begin_byte,
                                            text_width, !wrapped_layout);
    selection_end_position =
        TextPosition(layout, selection_end_byte, text_width, !wrapped_layout);
  }
  if (success && !text.value.empty()) {
    rendered = SDL_CreateSurface(std::max(1, text_width),
                                 std::max(1, text_height),
                                 SDL_PIXELFORMAT_RGBA32);
    success = rendered != nullptr &&
              TTF_SetTextColor(layout, text_color.r, text_color.g, text_color.b,
                               255) &&
              TTF_DrawSurfaceText(layout, 0, 0, rendered);
  }

  Status status = Status::Ok();
  int text_offset = 0;
  int text_y = clip_top;
  if (!success) {
    status = RasterizationFailure("unable to rasterize Unicode text");
  } else {
    const int caret_margin = std::max(1, Scaled(3.0F, scale));
    if (!wrapped_layout && text_width <= available_width) {
      if (presentation.x_alignment == 1) {
        text_offset = available_width - text_width;
      } else if (presentation.x_alignment == 2) {
        text_offset = (available_width - text_width) / 2;
      }
    } else if (!wrapped_layout &&
               caret_position.x > available_width - caret_margin) {
      text_offset = available_width - caret_margin - caret_position.x;
    }
    if (presentation.y_alignment == 1) {
      text_y = std::max(clip_top, (frame_height - text_height) / 2);
    } else if (presentation.y_alignment == 2) {
      text_y = std::max(clip_top, clip_bottom - text_height);
    }
    const auto fill_highlight = [&](int left, int top, int right, int bottom) {
      const int bounded_left = std::clamp(left, clip_left, clip_right);
      const int bounded_right = std::clamp(right, bounded_left, clip_right);
      const int bounded_top = std::clamp(top, clip_top, clip_bottom);
      const int bounded_bottom = std::clamp(bottom, bounded_top, clip_bottom);
      for (int y = bounded_top; y < bounded_bottom; ++y) {
        for (int x = bounded_left; x < bounded_right; ++x) {
          SetPixel(&candidate, frame_width, x, y, 65, 132, 228, 112);
        }
      }
    };
    if (selection_begin_byte != selection_end_byte) {
      if (wrapped_layout && layout != nullptr) {
        const std::size_t range_bytes =
            selection_end_byte - selection_begin_byte;
        const int range_offset = static_cast<int>(std::min<std::size_t>(
            selection_begin_byte,
            static_cast<std::size_t>(std::numeric_limits<int>::max())));
        const int range_length = static_cast<int>(std::min<std::size_t>(
            range_bytes,
            static_cast<std::size_t>(std::numeric_limits<int>::max())));
        int count = 0;
        TTF_SubString** substrings = TTF_GetTextSubStringsForRange(
            layout, range_offset, range_length, &count);
        if (substrings != nullptr) {
          for (int index = 0; index < count; ++index) {
            const TTF_SubString* substring = substrings[index];
            if (substring == nullptr) {
              continue;
            }
            fill_highlight(
                clip_left + text_offset + substring->rect.x,
                text_y + substring->rect.y,
                clip_left + text_offset + substring->rect.x + substring->rect.w,
                text_y + substring->rect.y + substring->rect.h);
          }
          SDL_free(substrings);
        }
      } else {
        fill_highlight(
            clip_left + text_offset +
                std::min(selection_begin_position.x, selection_end_position.x),
            text_y,
            clip_left + text_offset +
                std::max(selection_begin_position.x, selection_end_position.x),
            text_y + text_height);
      }
    }
    if (rendered != nullptr) {
      BlendSurface(rendered, clip_left + text_offset, text_y, clip_left,
                   clip_top, clip_right, clip_bottom, frame_width, text_color.a,
                   &candidate);
    }
    const int caret_width = std::max(1, Scaled(1.0F, scale));
    const int caret_screen_x =
        std::clamp(clip_left + text_offset + caret_position.x, clip_left,
                   std::max(clip_left, clip_right - caret_width - 1));
    const int caret_line_height =
        caret_position.height > 0
            ? caret_position.height
            : std::max(1, TTF_GetFontHeight(font));
    const int caret_height =
        std::min(std::max(Scaled(12.0F, scale), caret_line_height),
                 clip_bottom - clip_top);
    const int caret_y =
        std::clamp(text_y + caret_position.y, clip_top,
                   std::max(clip_top, clip_bottom - caret_height));
    for (int y = caret_y; y < caret_y + caret_height; ++y) {
      for (int x = caret_screen_x; x < caret_screen_x + caret_width; ++x) {
        SetPixel(&candidate, frame_width, x, y, text_color.r, text_color.g,
                 text_color.b, text_color.a);
      }
    }
  }

  if (rendered != nullptr) {
    SDL_DestroySurface(rendered);
  }
  if (!status.ok()) {
    if (layout != nullptr) {
      TTF_DestroyText(layout);
    }
    SecureClear(&candidate);
    return status;
  }
  cache.layout = layout;
  cache.text_offset = clip_left + text_offset;
  cache.text_y = text_y;
  cache.scale = scale;
  cache.window_begin = window_begin_byte;
  cache.window_bytes = text.value.size();
  SecureClear(&rgba_);
  rgba_ = std::move(candidate);
  raster_width_ = frame_width;
  raster_height_ = frame_height;
  raster_revision_ = state_revision_;
  if (!ready_logged_) {
    ready_logged_ = true;
    std::fprintf(stderr,
                 "  [input] same-surface TextBox raster ready "
                 "(SDL3_ttf/FreeType/HarfBuzz)\n");
  }
  return Status::Ok();
}

void RobloxTextSurfaceOverlay::ClearFrameLocked() {
  SecureClear(&rgba_);
  raster_width_ = 0;
  raster_height_ = 0;
  raster_revision_ = 0;
  if (raster_cache_ != nullptr) {
    raster_cache_->CloseLayout();
  }
}

void RobloxTextSurfaceOverlay::RecordFailureLocked(Status status) {
  if (status.ok()) {
    return;
  }
  failure_ = std::move(status);
  if (!failure_logged_) {
    failure_logged_ = true;
    std::fprintf(stderr, "  [input] same-surface TextBox raster failed: %s\n",
                 failure_.message().c_str());
  }
}

}  // namespace runtime
}  // namespace blox7

extern "C" bool blox7_text_overlay_may_present() {
  return blox7::runtime::g_overlay_may_present.load(
      std::memory_order_acquire);
}

extern "C" bool blox7_text_overlay_query(
    Blox7TextOverlayFrameInfo* frame) {
  std::lock_guard<std::mutex> lock(blox7::runtime::g_active_overlay_mutex);
  return blox7::runtime::g_active_overlay != nullptr &&
         blox7::runtime::g_active_overlay->QueryFrame(frame);
}

extern "C" bool blox7_text_overlay_copy(std::uint64_t revision, void* rgba,
                                           std::size_t rgba_capacity) {
  std::lock_guard<std::mutex> lock(blox7::runtime::g_active_overlay_mutex);
  return blox7::runtime::g_active_overlay != nullptr &&
         blox7::runtime::g_active_overlay->CopyFrame(revision, rgba,
                                                        rgba_capacity);
}

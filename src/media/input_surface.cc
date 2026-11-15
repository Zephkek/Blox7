#include "media/input_surface.h"

#include <cstdio>
#include <new>
#include <unordered_map>
#include <utility>

#include "blox7/media/encoder_surface.h"

extern "C" {
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

namespace blox7::media {
namespace {

struct Entry {
  std::shared_ptr<InputSurface> surface;
  int refs = 0;
};

std::mutex& RegistryMutex() {
  static std::mutex mutex;
  return mutex;
}

std::unordered_map<const void*, Entry>& Registry() {
  static std::unordered_map<const void*, Entry> registry;
  return registry;
}

std::shared_ptr<InputSurface> Find(const void* window) {
  std::lock_guard<std::mutex> lock(RegistryMutex());
  const auto found = Registry().find(window);
  return found != Registry().end() ? found->second.surface : nullptr;
}

void SurfaceSize(const void* window, std::int32_t* width,
                 std::int32_t* height) {
  const std::shared_ptr<InputSurface> surface = Find(window);
  if (width != nullptr) {
    *width = surface != nullptr ? surface->width() : 0;
  }
  if (height != nullptr) {
    *height = surface != nullptr ? surface->height() : 0;
  }
}

void SurfaceAcquire(const void* window) {
  std::lock_guard<std::mutex> lock(RegistryMutex());
  const auto found = Registry().find(window);
  if (found != Registry().end()) {
    ++found->second.refs;
  }
}

void SurfaceRelease(const void* window) { InputSurface::Release(window); }

void SurfaceQueue(const void* window, std::int64_t time_us) {
  if (const std::shared_ptr<InputSurface> surface = Find(window)) {
    surface->Queue(time_us);
  }
}

void SurfaceSubmit(const void* window, const void* pixels, std::int32_t stride,
                   std::int32_t format, std::int64_t time_us) {
  if (const std::shared_ptr<InputSurface> surface = Find(window)) {
    surface->Submit(pixels, stride, format, time_us);
  }
}

const Blox7EncoderSurface kOps = {SurfaceSize, SurfaceAcquire, SurfaceRelease,
                                  SurfaceQueue, SurfaceSubmit};

}  // namespace

InputSurface::InputSurface(std::int32_t width, std::int32_t height,
                           const PictureLayout& layout)
    : width_(width), height_(height), layout_(layout) {}

InputSurface::~InputSurface() { sws_freeContext(scaler_); }

std::shared_ptr<InputSurface> InputSurface::Create(
    std::int32_t width, std::int32_t height, const PictureLayout& layout) {
  InputSurface* created =
      new (std::nothrow) InputSurface(width, height, layout);
  if (created == nullptr) {
    return nullptr;
  }
  std::shared_ptr<InputSurface> surface(created);
  std::lock_guard<std::mutex> lock(RegistryMutex());
  Registry()[created] = Entry{surface, 2};
  return surface;
}

void InputSurface::Release(const void* window) {
  std::shared_ptr<InputSurface> last;
  {
    std::lock_guard<std::mutex> lock(RegistryMutex());
    const auto found = Registry().find(window);
    if (found == Registry().end() || --found->second.refs > 0) {
      return;
    }
    last = std::move(found->second.surface);
    Registry().erase(found);
  }
}

void InputSurface::Attach(FrameSink* sink) {
  std::lock_guard<std::mutex> lock(mutex_);
  sink_ = sink;
}

void InputSurface::Detach(FrameSink* sink) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (sink_ == sink) {
    sink_ = nullptr;
  }
}

void InputSurface::Queue(std::int64_t time_us) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (sink_ != nullptr) {
    sink_->QueueFrame(time_us);
  }
}

void InputSurface::Submit(const void* pixels, std::int32_t stride,
                          std::int32_t format, std::int64_t time_us) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (sink_ == nullptr) {
    return;
  }
  if (pixels == nullptr || stride < layout_.width * 4) {
    sink_->DropFrame(time_us);
    return;
  }
  const AVPixelFormat source =
      format == BLOX7_ENCODER_PIXELS_BGRA ? AV_PIX_FMT_BGRA : AV_PIX_FMT_RGBA;
  if (scaler_ == nullptr || scaler_format_ != source) {
    sws_freeContext(scaler_);
    scaler_ = sws_getContext(layout_.width, layout_.height, source,
                             layout_.width, layout_.height, AV_PIX_FMT_YUV420P,
                             SWS_BILINEAR, nullptr, nullptr, nullptr);
    scaler_format_ = source;
    if (scaler_ == nullptr) {
      sink_->DropFrame(time_us);
      if (!warned_) {
        warned_ = true;
        std::fprintf(stderr,
                     "  [mediacodec] cannot convert %dx%d frames for the "
                     "encoder\n",
                     layout_.width, layout_.height);
      }
      return;
    }
    const int* bt709 = sws_getCoefficients(SWS_CS_ITU709);
    sws_setColorspaceDetails(scaler_, bt709, 1, bt709, 0, 0, 1 << 16,
                             1 << 16);
  }
  std::vector<std::uint8_t> frame = sink_->TakeFrameBuffer(layout_.Size());
  if (frame.size() < layout_.Size()) {
    sink_->DropFrame(time_us);
    return;
  }
  std::uint8_t* const planes[4] = {
      frame.data(), frame.data() + layout_.LumaSize(),
      frame.data() + layout_.LumaSize() + layout_.ChromaSize(), nullptr};
  const int strides[4] = {layout_.luma_stride, layout_.chroma_stride,
                          layout_.chroma_stride, 0};
  const std::uint8_t* const source_planes[4] = {
      static_cast<const std::uint8_t*>(pixels), nullptr, nullptr, nullptr};
  const int source_strides[4] = {stride, 0, 0, 0};
  sws_scale(scaler_, source_planes, source_strides, 0, layout_.height, planes,
            strides);
  sink_->PushFrame(std::move(frame), time_us);
}

}  // namespace blox7::media

extern "C" __attribute__((visibility("default"))) const Blox7EncoderSurface*
blox7_media_encoder_surface(const void* window) {
  if (window == nullptr) {
    return nullptr;
  }
  std::lock_guard<std::mutex> lock(blox7::media::RegistryMutex());
  return blox7::media::Registry().count(window) != 0 ? &blox7::media::kOps
                                                       : nullptr;
}

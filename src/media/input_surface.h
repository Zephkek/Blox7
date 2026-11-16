#ifndef BLOX7_MEDIA_INPUT_SURFACE_H_
#define BLOX7_MEDIA_INPUT_SURFACE_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include "media/encoding.h"

struct ANativeWindow;
struct SwsContext;

namespace blox7::media {

class FrameSink {
 public:
  virtual void QueueFrame(std::int64_t time_us) = 0;
  virtual void DropFrame(std::int64_t time_us) = 0;
  virtual std::vector<std::uint8_t> TakeFrameBuffer(std::size_t size) = 0;
  virtual void PushFrame(std::vector<std::uint8_t> frame,
                         std::int64_t time_us) = 0;

 protected:
  ~FrameSink() = default;
};

class InputSurface final {
 public:
  static std::shared_ptr<InputSurface> Create(std::int32_t width,
                                              std::int32_t height,
                                              const PictureLayout& layout);
  static void Release(const void* window);
  ~InputSurface();
  InputSurface(const InputSurface&) = delete;
  InputSurface& operator=(const InputSurface&) = delete;

  ANativeWindow* window() { return reinterpret_cast<ANativeWindow*>(this); }
  std::int32_t width() const { return width_; }
  std::int32_t height() const { return height_; }
  const PictureLayout& layout() const { return layout_; }

  void Attach(FrameSink* sink);
  void Detach(FrameSink* sink);
  void Queue(std::int64_t time_us);
  void Submit(const void* pixels, std::int32_t stride, std::int32_t format,
              std::int64_t time_us);
  void SubmitPlanes(const void* data, std::int64_t time_us);

 private:
  InputSurface(std::int32_t width, std::int32_t height,
               const PictureLayout& layout);
  std::vector<std::uint8_t> TakeFrame(std::int64_t time_us);
  void DeliverFrame(std::vector<std::uint8_t> frame, std::int64_t time_us);
  void DropFrame(std::int64_t time_us);

  const std::int32_t width_;
  const std::int32_t height_;
  const PictureLayout layout_;
  std::mutex sink_mutex_;
  FrameSink* sink_ = nullptr;
  std::mutex convert_mutex_;
  SwsContext* scaler_ = nullptr;
  int scaler_format_ = -1;
  bool warned_ = false;
};

}  // namespace blox7::media

#endif  // BLOX7_MEDIA_INPUT_SURFACE_H_

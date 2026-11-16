#ifndef BLOX7_MEDIA_VIDEO_ENCODER_H_
#define BLOX7_MEDIA_VIDEO_ENCODER_H_

#include <cstdint>
#include <memory>
#include <vector>

#include "media/encoding.h"
#include "media/media_format.h"

namespace blox7::media {

struct AvcSettings {
  std::int32_t width = 0;
  std::int32_t height = 0;
  std::int32_t bitrate = 0;
  float frame_rate = 0;
  float key_frame_interval = 0;
};

class VideoEncoder {
 public:
  virtual ~VideoEncoder() = default;

  virtual std::int32_t width() const = 0;
  virtual std::int32_t height() const = 0;
  virtual const PictureLayout& layout() const = 0;

  virtual bool Encode(const std::uint8_t* picture, std::int64_t time_us,
                      std::vector<EncodedPacket>* out) = 0;
  virtual bool Drain(std::vector<EncodedPacket>* out) = 0;
  virtual void RequestKeyFrame() = 0;
  virtual void Interrupt() = 0;
  virtual MediaFormat OutputFormat() const = 0;
};

std::unique_ptr<VideoEncoder> CreateAvcEncoder(const MediaFormat& format);

MediaFormat AvcOutputFormat(const AvcSettings& settings,
                            const PictureLayout& layout,
                            const std::vector<std::uint8_t>& sps,
                            const std::vector<std::uint8_t>& pps);

#if defined(BLOX7_WIN32)
std::unique_ptr<VideoEncoder> CreateHardwareAvcEncoder(
    const AvcSettings& settings);
#endif

}  // namespace blox7::media

#endif  // BLOX7_MEDIA_VIDEO_ENCODER_H_

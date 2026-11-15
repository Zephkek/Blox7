#ifndef BLOX7_MEDIA_H264_ENCODER_H_
#define BLOX7_MEDIA_H264_ENCODER_H_

#include <cstdint>
#include <memory>
#include <vector>

#include "media/encoding.h"
#include "media/media_format.h"

class ISVCEncoder;

namespace blox7::media {

class H264Encoder final {
 public:
  static std::unique_ptr<H264Encoder> Create(const MediaFormat& format);
  ~H264Encoder();
  H264Encoder(const H264Encoder&) = delete;
  H264Encoder& operator=(const H264Encoder&) = delete;

  std::int32_t width() const { return width_; }
  std::int32_t height() const { return height_; }
  const PictureLayout& layout() const { return layout_; }

  bool Encode(const std::uint8_t* picture, std::int64_t time_us,
              std::vector<EncodedPacket>* out);
  void RequestKeyFrame();
  MediaFormat OutputFormat() const;

 private:
  H264Encoder(ISVCEncoder* encoder, std::int32_t width, std::int32_t height,
              std::int32_t bitrate, float frame_rate);

  ISVCEncoder* const encoder_;
  const std::int32_t width_;
  const std::int32_t height_;
  const PictureLayout layout_;
  const std::int32_t bitrate_;
  const float frame_rate_;
  std::vector<std::uint8_t> sps_;
  std::vector<std::uint8_t> pps_;
  bool config_sent_ = false;
};

}  // namespace blox7::media

#endif  // BLOX7_MEDIA_H264_ENCODER_H_

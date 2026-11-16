#ifndef BLOX7_MEDIA_H264_ENCODER_H_
#define BLOX7_MEDIA_H264_ENCODER_H_

#include <cstdint>
#include <memory>
#include <vector>

#include "media/encoding.h"
#include "media/media_format.h"
#include "media/video_encoder.h"

class ISVCEncoder;

namespace blox7::media {

class H264Encoder final : public VideoEncoder {
 public:
  static std::unique_ptr<H264Encoder> Create(const AvcSettings& settings);
  ~H264Encoder() override;
  H264Encoder(const H264Encoder&) = delete;
  H264Encoder& operator=(const H264Encoder&) = delete;

  std::int32_t width() const override { return settings_.width; }
  std::int32_t height() const override { return settings_.height; }
  const PictureLayout& layout() const override { return layout_; }

  bool Encode(const std::uint8_t* picture, std::int64_t time_us,
              std::vector<EncodedPacket>* out) override;
  bool Drain(std::vector<EncodedPacket>* out) override;
  void RequestKeyFrame() override;
  void Interrupt() override {}
  MediaFormat OutputFormat() const override;

 private:
  H264Encoder(ISVCEncoder* encoder, const AvcSettings& settings);

  ISVCEncoder* const encoder_;
  const AvcSettings settings_;
  const PictureLayout layout_;
  std::vector<std::uint8_t> sps_;
  std::vector<std::uint8_t> pps_;
  bool config_sent_ = false;
};

}  // namespace blox7::media

#endif  // BLOX7_MEDIA_H264_ENCODER_H_

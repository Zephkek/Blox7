#ifndef BLOX7_MEDIA_FFMPEG_ENCODER_H_
#define BLOX7_MEDIA_FFMPEG_ENCODER_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "media/encoding.h"
#include "media/media_format.h"

struct AVCodecContext;
struct AVFrame;
struct AVPacket;

namespace blox7::media {

class FfmpegEncoder final {
 public:
  static std::unique_ptr<FfmpegEncoder> Create(const MediaFormat& format);
  ~FfmpegEncoder();
  FfmpegEncoder(const FfmpegEncoder&) = delete;
  FfmpegEncoder& operator=(const FfmpegEncoder&) = delete;

  bool Encode(const std::uint8_t* data, std::size_t size, std::int64_t time_us,
              std::vector<EncodedPacket>* out);
  bool Finish(std::vector<EncodedPacket>* out);
  bool Reset();
  MediaFormat OutputFormat() const;

 private:
  FfmpegEncoder(int sample_rate, int channels, int bitrate);
  bool Open();
  void Close();
  bool SendFrame(const std::int16_t* samples, int count,
                 std::vector<EncodedPacket>* out);
  bool Drain(std::vector<EncodedPacket>* out);
  void AddConfig(std::vector<EncodedPacket>* out);

  const int sample_rate_;
  const int channels_;
  const int bitrate_;
  AVCodecContext* context_ = nullptr;
  AVFrame* frame_ = nullptr;
  AVPacket* packet_ = nullptr;
  int frame_size_ = 1024;
  std::vector<std::int16_t> pending_;
  bool started_ = false;
  std::int64_t start_us_ = 0;
  std::int64_t samples_sent_ = 0;
  std::int64_t packets_out_ = 0;
  std::vector<std::uint8_t> config_;
  bool config_sent_ = false;
};

}  // namespace blox7::media

#endif  // BLOX7_MEDIA_FFMPEG_ENCODER_H_

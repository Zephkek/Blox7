#ifndef BLOX7_MEDIA_FFMPEG_DECODER_H_
#define BLOX7_MEDIA_FFMPEG_DECODER_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "media/codec_names.h"
#include "media/media_format.h"

struct AVCodec;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;

namespace blox7::media {

struct OutputLayout {
  std::int32_t width = 0;
  std::int32_t height = 0;
  std::int32_t stride = 0;
  std::int32_t sample_rate = 0;
  std::int32_t channels = 0;

  bool operator==(const OutputLayout& other) const {
    return width == other.width && height == other.height &&
           stride == other.stride && sample_rate == other.sample_rate &&
           channels == other.channels;
  }
};

class FfmpegDecoder final {
 public:
  enum class Result { kOk, kAgain, kEnd, kError };

  static std::unique_ptr<FfmpegDecoder> Create(CodecKind kind,
                                               const MediaFormat& format);
  ~FfmpegDecoder();
  FfmpegDecoder(const FfmpegDecoder&) = delete;
  FfmpegDecoder& operator=(const FfmpegDecoder&) = delete;

  bool SetConfig(const std::uint8_t* data, std::size_t size);
  Result Send(const std::uint8_t* data, std::size_t size,
              std::int64_t time_us);
  void SendEnd();
  Result Receive();
  void Flush();

  OutputLayout Layout() const;
  std::size_t OutputSize() const;
  std::int64_t OutputTime() const;
  void CopyOutput(std::uint8_t* target) const;

 private:
  FfmpegDecoder(CodecKind kind, const AVCodec* codec, bool low_latency,
                int threads);
  bool Open(const std::vector<std::uint8_t>& extradata);
  void Close();
  void CopyVideo(std::uint8_t* target) const;
  void CopyAudio(std::uint8_t* target) const;

  const CodecKind kind_;
  const AVCodec* const codec_;
  const bool low_latency_;
  const int threads_;
  AVCodecContext* context_ = nullptr;
  AVFrame* frame_ = nullptr;
  AVPacket* packet_ = nullptr;
  bool open_ = false;
};

}  // namespace blox7::media

#endif  // BLOX7_MEDIA_FFMPEG_DECODER_H_

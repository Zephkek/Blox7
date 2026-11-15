#include "media/ffmpeg_decoder.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>
#include <mutex>
#include <thread>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#include <libavutil/log.h>
#include <libavutil/mem.h>
#include <libavutil/pixdesc.h>
#include <libavutil/samplefmt.h>
}

namespace blox7::media {
namespace {

constexpr std::uint8_t kStartCode[] = {0, 0, 0, 1};

bool HasStartCode(const MediaFormat::Buffer& data) {
  return (data.size() >= 3 && data[0] == 0 && data[1] == 0 && data[2] == 1) ||
         (data.size() >= 4 && data[0] == 0 && data[1] == 0 && data[2] == 0 &&
          data[3] == 1);
}

int ChromaSize(int length) { return (length + 1) / 2; }

bool Readable(const AVPixFmtDescriptor* desc) {
  constexpr std::uint64_t kUnreadable =
      AV_PIX_FMT_FLAG_BE | AV_PIX_FMT_FLAG_PAL | AV_PIX_FMT_FLAG_BITSTREAM |
      AV_PIX_FMT_FLAG_HWACCEL | AV_PIX_FMT_FLAG_RGB | AV_PIX_FMT_FLAG_BAYER |
      AV_PIX_FMT_FLAG_FLOAT | AV_PIX_FMT_FLAG_XYZ;
  return desc != nullptr && (desc->flags & kUnreadable) == 0 &&
         desc->comp[0].depth >= 8 && desc->comp[0].depth <= 16;
}

bool IsPlanar420(const AVPixFmtDescriptor* desc) {
  return desc->comp[0].depth == 8 && desc->nb_components >= 3 &&
         desc->log2_chroma_w == 1 && desc->log2_chroma_h == 1 &&
         desc->comp[0].step == 1 && desc->comp[1].step == 1 &&
         desc->comp[2].step == 1 && desc->comp[1].plane != desc->comp[2].plane;
}

std::uint32_t SampleAt(const std::uint8_t* row,
                       const AVComponentDescriptor& comp, int x) {
  const std::uint8_t* at =
      row + comp.offset + static_cast<std::ptrdiff_t>(x) * comp.step;
  std::uint32_t value = at[0];
  if (comp.depth + comp.shift > 8) {
    std::uint16_t wide;
    std::memcpy(&wide, at, sizeof wide);
    value = wide;
  }
  return (value >> comp.shift) & ((1u << comp.depth) - 1);
}

std::uint8_t To8Bit(std::uint32_t value, int shift) {
  if (shift > 0) {
    value = (value + (1u << (shift - 1))) >> shift;
  }
  return static_cast<std::uint8_t>(std::min<std::uint32_t>(value, 255));
}

std::int16_t ToPcm16(double value) {
  return static_cast<std::int16_t>(
      std::lrint(std::clamp(value * 32768.0, -32768.0, 32767.0)));
}

int ThreadCount(const MediaFormat& format) {
  std::int32_t width = 0;
  std::int32_t height = 0;
  std::int32_t max_width = 0;
  std::int32_t max_height = 0;
  format.GetInt32("width", &width);
  format.GetInt32("height", &height);
  format.GetInt32("max-width", &max_width);
  format.GetInt32("max-height", &max_height);
  const std::int64_t pixels =
      std::int64_t{std::clamp(std::max(width, max_width), 0, 16384)} *
      std::clamp(std::max(height, max_height), 0, 16384);
  const int cores =
      static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
  const std::int64_t wanted =
      std::max<std::int64_t>(pixels * 3 / (1920 * 1080), 2);
  return static_cast<int>(std::min<std::int64_t>(wanted, std::min(cores, 16)));
}

}  // namespace

std::unique_ptr<FfmpegDecoder> FfmpegDecoder::Create(
    CodecKind kind, const MediaFormat& format) {
  static std::once_flag quiet;
  std::call_once(quiet, [] { av_log_set_level(AV_LOG_FATAL); });
  const AVCodecID id = kind == CodecKind::kAvc    ? AV_CODEC_ID_H264
                       : kind == CodecKind::kHevc ? AV_CODEC_ID_HEVC
                                                  : AV_CODEC_ID_AAC;
  const AVCodec* codec = avcodec_find_decoder(id);
  if (codec == nullptr) {
    return nullptr;
  }
  std::int32_t low_latency = 0;
  format.GetInt32("low-latency", &low_latency);
  std::vector<std::uint8_t> extradata;
  for (const char* key : {"csd-0", "csd-1", "csd-2"}) {
    const MediaFormat::Buffer* csd = format.GetBuffer(key);
    if (csd == nullptr || csd->empty()) {
      continue;
    }
    if (kind != CodecKind::kAac && !HasStartCode(*csd)) {
      extradata.insert(extradata.end(), std::begin(kStartCode),
                       std::end(kStartCode));
    }
    extradata.insert(extradata.end(), csd->begin(), csd->end());
    if (kind == CodecKind::kAac) {
      break;
    }
  }
  std::unique_ptr<FfmpegDecoder> decoder(new FfmpegDecoder(
      kind, codec, low_latency != 0, ThreadCount(format)));
  if (!decoder->Open(extradata)) {
    return nullptr;
  }
  return decoder;
}

FfmpegDecoder::FfmpegDecoder(CodecKind kind, const AVCodec* codec,
                             bool low_latency, int threads)
    : kind_(kind),
      codec_(codec),
      low_latency_(low_latency),
      threads_(threads) {}

FfmpegDecoder::~FfmpegDecoder() { Close(); }

bool FfmpegDecoder::Open(const std::vector<std::uint8_t>& extradata) {
  context_ = avcodec_alloc_context3(codec_);
  frame_ = av_frame_alloc();
  packet_ = av_packet_alloc();
  if (context_ == nullptr || frame_ == nullptr || packet_ == nullptr) {
    return false;
  }
  context_->pkt_timebase = AVRational{1, 1000000};
  if (!extradata.empty()) {
    context_->extradata = static_cast<std::uint8_t*>(
        av_mallocz(extradata.size() + AV_INPUT_BUFFER_PADDING_SIZE));
    if (context_->extradata == nullptr) {
      return false;
    }
    std::memcpy(context_->extradata, extradata.data(), extradata.size());
    context_->extradata_size = static_cast<int>(extradata.size());
  }
  if (kind_ != CodecKind::kAac) {
    context_->thread_count = threads_;
    context_->thread_type =
        low_latency_ ? FF_THREAD_SLICE : FF_THREAD_FRAME | FF_THREAD_SLICE;
    context_->flags |= AV_CODEC_FLAG_UNALIGNED;
    if (low_latency_) {
      context_->flags |= AV_CODEC_FLAG_LOW_DELAY;
    } else {
      context_->strict_std_compliance = FF_COMPLIANCE_STRICT;
    }
  }
  open_ = avcodec_open2(context_, codec_, nullptr) == 0;
  return open_;
}

void FfmpegDecoder::Close() {
  open_ = false;
  av_packet_free(&packet_);
  av_frame_free(&frame_);
  avcodec_free_context(&context_);
}

bool FfmpegDecoder::SetConfig(const std::uint8_t* data, std::size_t size) {
  Close();
  return Open(std::vector<std::uint8_t>(data, data + size));
}

FfmpegDecoder::Result FfmpegDecoder::Send(const std::uint8_t* data,
                                          std::size_t size,
                                          std::int64_t time_us) {
  if (!open_) {
    return Result::kError;
  }
  packet_->data = const_cast<std::uint8_t*>(data);
  packet_->size = static_cast<int>(size);
  packet_->pts = time_us;
  const int result = avcodec_send_packet(context_, packet_);
  packet_->data = nullptr;
  packet_->size = 0;
  if (result == 0) {
    return Result::kOk;
  }
  if (result == AVERROR(EAGAIN)) {
    return Result::kAgain;
  }
  return result == AVERROR_EOF ? Result::kEnd : Result::kError;
}

void FfmpegDecoder::SendEnd() {
  if (open_) {
    avcodec_send_packet(context_, nullptr);
  }
}

FfmpegDecoder::Result FfmpegDecoder::Receive() {
  if (!open_) {
    return Result::kEnd;
  }
  av_frame_unref(frame_);
  const int result = avcodec_receive_frame(context_, frame_);
  if (result == 0) {
    return Result::kOk;
  }
  if (result == AVERROR(EAGAIN)) {
    return Result::kAgain;
  }
  return result == AVERROR_EOF ? Result::kEnd : Result::kError;
}

void FfmpegDecoder::Flush() {
  if (open_) {
    av_frame_unref(frame_);
    avcodec_flush_buffers(context_);
  }
}

OutputLayout FfmpegDecoder::Layout() const {
  OutputLayout layout;
  if (kind_ == CodecKind::kAac) {
    layout.sample_rate = frame_->sample_rate;
    layout.channels = frame_->ch_layout.nb_channels;
  } else {
    layout.width = frame_->width;
    layout.height = frame_->height;
    layout.stride = frame_->width;
  }
  return layout;
}

std::size_t FfmpegDecoder::OutputSize() const {
  if (kind_ == CodecKind::kAac) {
    return static_cast<std::size_t>(frame_->nb_samples) *
           static_cast<std::size_t>(frame_->ch_layout.nb_channels) * 2;
  }
  return static_cast<std::size_t>(frame_->width) * frame_->height +
         2 * static_cast<std::size_t>(ChromaSize(frame_->width)) *
             ChromaSize(frame_->height);
}

std::int64_t FfmpegDecoder::OutputTime() const {
  if (frame_->best_effort_timestamp != AV_NOPTS_VALUE) {
    return frame_->best_effort_timestamp;
  }
  return frame_->pts != AV_NOPTS_VALUE ? frame_->pts : 0;
}

void FfmpegDecoder::CopyOutput(std::uint8_t* target) const {
  if (kind_ == CodecKind::kAac) {
    CopyAudio(target);
  } else {
    CopyVideo(target);
  }
}

void FfmpegDecoder::CopyVideo(std::uint8_t* target) const {
  const int width = frame_->width;
  const int height = frame_->height;
  const int chroma_width = ChromaSize(width);
  const int chroma_height = ChromaSize(height);
  const std::size_t luma_bytes = static_cast<std::size_t>(width) * height;
  const std::size_t chroma_bytes =
      static_cast<std::size_t>(chroma_width) * chroma_height;
  std::uint8_t* const planes[3] = {target, target + luma_bytes,
                                   target + luma_bytes + chroma_bytes};
  const AVPixFmtDescriptor* desc =
      av_pix_fmt_desc_get(static_cast<AVPixelFormat>(frame_->format));
  if (!Readable(desc)) {
    std::memset(planes[0], 0, luma_bytes);
    std::memset(planes[1], 128, 2 * chroma_bytes);
    return;
  }
  if (IsPlanar420(desc)) {
    const int widths[3] = {width, chroma_width, chroma_width};
    const int heights[3] = {height, chroma_height, chroma_height};
    for (int c = 0; c < 3; ++c) {
      const int plane = desc->comp[c].plane;
      for (int y = 0; y < heights[c]; ++y) {
        std::memcpy(planes[c] + static_cast<std::size_t>(y) * widths[c],
                    frame_->data[plane] +
                        static_cast<std::ptrdiff_t>(y) * frame_->linesize[plane],
                    widths[c]);
      }
    }
    return;
  }
  const int shift = desc->comp[0].depth - 8;
  const AVComponentDescriptor& luma = desc->comp[0];
  for (int y = 0; y < height; ++y) {
    const std::uint8_t* row =
        frame_->data[luma.plane] +
        static_cast<std::ptrdiff_t>(y) * frame_->linesize[luma.plane];
    std::uint8_t* out = planes[0] + static_cast<std::size_t>(y) * width;
    for (int x = 0; x < width; ++x) {
      out[x] = To8Bit(SampleAt(row, luma, x), shift);
    }
  }
  if (desc->nb_components < 3) {
    std::memset(planes[1], 128, 2 * chroma_bytes);
    return;
  }
  const int sub_w = desc->log2_chroma_w;
  const int sub_h = desc->log2_chroma_h;
  const int source_width = AV_CEIL_RSHIFT(width, sub_w);
  const int source_height = AV_CEIL_RSHIFT(height, sub_h);
  for (int c = 1; c < 3; ++c) {
    const AVComponentDescriptor& comp = desc->comp[c];
    for (int y = 0; y < chroma_height; ++y) {
      const int y0 = std::min((2 * y) >> sub_h, source_height - 1);
      const int y1 = std::min((2 * y + 1) >> sub_h, source_height - 1);
      const std::uint8_t* row0 =
          frame_->data[comp.plane] +
          static_cast<std::ptrdiff_t>(y0) * frame_->linesize[comp.plane];
      const std::uint8_t* row1 =
          frame_->data[comp.plane] +
          static_cast<std::ptrdiff_t>(y1) * frame_->linesize[comp.plane];
      std::uint8_t* out =
          planes[c] + static_cast<std::size_t>(y) * chroma_width;
      for (int x = 0; x < chroma_width; ++x) {
        const int x0 = std::min((2 * x) >> sub_w, source_width - 1);
        const int x1 = std::min((2 * x + 1) >> sub_w, source_width - 1);
        const std::uint32_t sum =
            SampleAt(row0, comp, x0) + SampleAt(row0, comp, x1) +
            SampleAt(row1, comp, x0) + SampleAt(row1, comp, x1);
        out[x] = To8Bit((sum + 2) >> 2, shift);
      }
    }
  }
}

void FfmpegDecoder::CopyAudio(std::uint8_t* target) const {
  const int channels = frame_->ch_layout.nb_channels;
  const int samples = frame_->nb_samples;
  const auto format = static_cast<AVSampleFormat>(frame_->format);
  const bool planar = av_sample_fmt_is_planar(format) != 0;
  const AVSampleFormat packed = av_get_packed_sample_fmt(format);
  for (int i = 0; i < samples; ++i) {
    for (int c = 0; c < channels; ++c) {
      const std::uint8_t* base = planar ? frame_->extended_data[c]
                                        : frame_->extended_data[0];
      const int at = planar ? i : i * channels + c;
      std::int16_t value = 0;
      if (packed == AV_SAMPLE_FMT_FLT) {
        float sample;
        std::memcpy(&sample, base + sizeof sample * at, sizeof sample);
        value = ToPcm16(sample);
      } else if (packed == AV_SAMPLE_FMT_DBL) {
        double sample;
        std::memcpy(&sample, base + sizeof sample * at, sizeof sample);
        value = ToPcm16(sample);
      } else if (packed == AV_SAMPLE_FMT_S16) {
        std::memcpy(&value, base + sizeof value * at, sizeof value);
      } else if (packed == AV_SAMPLE_FMT_S32) {
        std::int32_t sample;
        std::memcpy(&sample, base + sizeof sample * at, sizeof sample);
        value = static_cast<std::int16_t>(sample >> 16);
      }
      std::memcpy(target + 2 * (static_cast<std::size_t>(i) * channels + c),
                  &value, sizeof value);
    }
  }
}

}  // namespace blox7::media

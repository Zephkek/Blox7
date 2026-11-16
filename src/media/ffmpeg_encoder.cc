#include "media/ffmpeg_encoder.h"

#include <media/NdkMediaCodec.h>

#include <cstdio>
#include <cstring>
#include <new>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#include <libavutil/log.h>
#include <libavutil/samplefmt.h>
}

namespace blox7::media {
namespace {

constexpr std::int32_t kDefaultBitrate = 64000;
constexpr std::int32_t kPcmEncoding16Bit = 2;
constexpr std::int32_t kAacProfileLow = 2;

}  // namespace

std::unique_ptr<FfmpegEncoder> FfmpegEncoder::Create(
    const MediaFormat& format) {
  av_log_set_level(AV_LOG_FATAL);
  const std::int32_t sample_rate = format.Int32Or("sample-rate", 0);
  const std::int32_t channels = format.Int32Or("channel-count", 0);
  const std::int32_t bitrate = format.Int32Or("bitrate", kDefaultBitrate);
  const std::int32_t pcm = format.Int32Or("pcm-encoding", kPcmEncoding16Bit);
  const std::int32_t profile = format.Int32Or("aac-profile", kAacProfileLow);
  if (sample_rate < 8000 || sample_rate > 96000 || channels < 1 ||
      channels > 8 || pcm != kPcmEncoding16Bit || profile != kAacProfileLow ||
      bitrate <= 0) {
    std::fprintf(stderr,
                 "  [mediacodec] no AAC encoder for %d Hz, %d channels, pcm "
                 "%d, profile %d\n",
                 sample_rate, channels, pcm, profile);
    return nullptr;
  }
  std::unique_ptr<FfmpegEncoder> encoder(
      new (std::nothrow) FfmpegEncoder(sample_rate, channels, bitrate));
  if (encoder == nullptr || !encoder->Open()) {
    return nullptr;
  }
  return encoder;
}

FfmpegEncoder::FfmpegEncoder(int sample_rate, int channels, int bitrate)
    : sample_rate_(sample_rate), channels_(channels), bitrate_(bitrate) {}

FfmpegEncoder::~FfmpegEncoder() { Close(); }

bool FfmpegEncoder::Open() {
  const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
  if (codec == nullptr) {
    return false;
  }
  context_ = avcodec_alloc_context3(codec);
  frame_ = av_frame_alloc();
  packet_ = av_packet_alloc();
  if (context_ == nullptr || frame_ == nullptr || packet_ == nullptr) {
    Close();
    return false;
  }
  context_->sample_fmt = AV_SAMPLE_FMT_FLTP;
  context_->sample_rate = sample_rate_;
  av_channel_layout_default(&context_->ch_layout, channels_);
  context_->bit_rate = bitrate_;
  context_->profile = AV_PROFILE_AAC_LOW;
  context_->time_base = AVRational{1, sample_rate_};
  context_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
  if (avcodec_open2(context_, codec, nullptr) < 0 ||
      context_->frame_size <= 0) {
    std::fprintf(stderr, "  [mediacodec] FFmpeg refused AAC at %d Hz\n",
                 sample_rate_);
    Close();
    return false;
  }
  frame_size_ = context_->frame_size;
  if (context_->extradata != nullptr && context_->extradata_size > 0) {
    config_.assign(context_->extradata,
                   context_->extradata + context_->extradata_size);
  }
  if (config_.size() > 2 && (config_[0] >> 3) == 2 &&
      (((config_[0] & 7) << 1) | (config_[1] >> 7)) != 15) {
    config_.resize(2);
  }
  return true;
}

void FfmpegEncoder::Close() {
  avcodec_free_context(&context_);
  av_frame_free(&frame_);
  av_packet_free(&packet_);
}

void FfmpegEncoder::AddConfig(std::vector<EncodedPacket>* out) {
  if (config_sent_ || config_.empty()) {
    return;
  }
  EncodedPacket config;
  config.data = config_;
  config.time_us = 0;
  config.flags = AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG;
  out->push_back(std::move(config));
  config_sent_ = true;
}

bool FfmpegEncoder::Encode(const std::uint8_t* data, std::size_t size,
                           std::int64_t time_us,
                           std::vector<EncodedPacket>* out) {
  const std::size_t count = size / (sizeof(std::int16_t) * channels_);
  if (count == 0) {
    return true;
  }
  if (!started_) {
    started_ = true;
    start_us_ = time_us;
  }
  AddConfig(out);
  const std::size_t old_size = pending_.size();
  pending_.resize(old_size + count * channels_);
  std::memcpy(pending_.data() + old_size, data,
              count * channels_ * sizeof(std::int16_t));
  const std::size_t frame_values =
      static_cast<std::size_t>(frame_size_) * channels_;
  std::size_t used = 0;
  bool ok = true;
  while (ok && pending_.size() - used >= frame_values) {
    ok = SendFrame(pending_.data() + used, frame_size_, out);
    used += frame_values;
  }
  pending_.erase(pending_.begin(),
                 pending_.begin() + static_cast<std::ptrdiff_t>(used));
  return ok;
}

bool FfmpegEncoder::Finish(std::vector<EncodedPacket>* out) {
  if (!started_ || context_ == nullptr) {
    return true;
  }
  AddConfig(out);
  bool ok = true;
  if (!pending_.empty()) {
    ok = SendFrame(pending_.data(),
                   static_cast<int>(pending_.size() / channels_), out);
    pending_.clear();
  }
  if (avcodec_send_frame(context_, nullptr) < 0) {
    return false;
  }
  return Drain(out) && ok;
}

bool FfmpegEncoder::Reset() {
  Close();
  pending_.clear();
  started_ = false;
  start_us_ = 0;
  samples_sent_ = 0;
  packets_out_ = 0;
  return Open();
}

bool FfmpegEncoder::SendFrame(const std::int16_t* samples, int count,
                              std::vector<EncodedPacket>* out) {
  frame_->nb_samples = count;
  frame_->format = AV_SAMPLE_FMT_FLTP;
  frame_->sample_rate = sample_rate_;
  if (av_channel_layout_copy(&frame_->ch_layout, &context_->ch_layout) < 0 ||
      av_frame_get_buffer(frame_, 0) < 0) {
    av_frame_unref(frame_);
    return false;
  }
  for (int channel = 0; channel < channels_; ++channel) {
    auto* plane = reinterpret_cast<float*>(frame_->extended_data[channel]);
    for (int index = 0; index < count; ++index) {
      plane[index] = static_cast<float>(samples[index * channels_ + channel]) /
                     32768.0f;
    }
  }
  frame_->pts = samples_sent_;
  samples_sent_ += count;
  const int sent = avcodec_send_frame(context_, frame_);
  av_frame_unref(frame_);
  return sent >= 0 && Drain(out);
}

bool FfmpegEncoder::Drain(std::vector<EncodedPacket>* out) {
  while (true) {
    const int received = avcodec_receive_packet(context_, packet_);
    if (received == AVERROR(EAGAIN) || received == AVERROR_EOF) {
      return true;
    }
    if (received < 0) {
      return false;
    }
    EncodedPacket packet;
    packet.data.assign(packet_->data, packet_->data + packet_->size);
    packet.time_us = start_us_ + packets_out_ * frame_size_ * 1000000 /
                                     sample_rate_;
    ++packets_out_;
    av_packet_unref(packet_);
    out->push_back(std::move(packet));
  }
}

MediaFormat FfmpegEncoder::OutputFormat() const {
  MediaFormat format;
  format.Set("sample-rate", static_cast<std::int32_t>(sample_rate_));
  format.Set("bitrate", static_cast<std::int32_t>(bitrate_));
  format.Set("channel-count", static_cast<std::int32_t>(channels_));
  format.Set("level", 0);
  format.Set("max-bitrate", static_cast<std::int32_t>(bitrate_));
  format.Set("mime", std::string("audio/mp4a-latm"));
  format.Set("profile", kAacProfileLow);
  if (config_sent_ && !config_.empty()) {
    format.Set("csd-0", config_);
  }
  return format;
}

}  // namespace blox7::media

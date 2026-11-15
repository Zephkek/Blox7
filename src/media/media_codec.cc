#include "media/media_codec.h"

#include <pthread.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <utility>

namespace blox7::media {
namespace {

constexpr std::size_t kInputSlots = 8;
constexpr std::size_t kOutputSlots = 8;
constexpr std::int32_t kColorFormatYuv420Planar = 19;
constexpr std::int32_t kPcmEncoding16Bit = 2;

std::int32_t Int32Or(const MediaFormat& format, const char* key,
                     std::int32_t fallback) {
  std::int32_t value = 0;
  return format.GetInt32(key, &value) ? value : fallback;
}

std::size_t InputCapacity(CodecKind kind, const MediaFormat& format) {
  const std::size_t requested = static_cast<std::size_t>(
      std::max<std::int32_t>(Int32Or(format, "max-input-size", 0), 0));
  if (kind == CodecKind::kAac) {
    return std::max<std::size_t>(requested, 64 * 1024);
  }
  const std::int64_t width = std::clamp(
      std::max(Int32Or(format, "max-width", 0), Int32Or(format, "width", 0)),
      0, 16384);
  const std::int64_t height = std::clamp(
      std::max(Int32Or(format, "max-height", 0), Int32Or(format, "height", 0)),
      0, 16384);
  const std::int64_t pixels =
      width > 0 && height > 0 ? width * height : std::int64_t{1920} * 1080;
  return std::max(requested,
                  static_cast<std::size_t>(std::max<std::int64_t>(
                      pixels * 3 / 2, std::int64_t{1} << 20)));
}

MediaFormat OutputFormatFor(CodecKind kind, const OutputLayout& layout) {
  MediaFormat format;
  if (kind == CodecKind::kAac) {
    format.Set("mime", std::string("audio/raw"));
    format.Set("sample-rate", layout.sample_rate);
    format.Set("channel-count", layout.channels);
    format.Set("pcm-encoding", kPcmEncoding16Bit);
    return format;
  }
  format.Set("mime", std::string("video/raw"));
  format.Set("width", layout.width);
  format.Set("height", layout.height);
  format.Set("color-format", kColorFormatYuv420Planar);
  format.Set("stride", layout.stride);
  format.Set("slice-height", layout.height);
  format.Set("crop-left", 0);
  format.Set("crop-top", 0);
  format.Set("crop-right", layout.width - 1);
  format.Set("crop-bottom", layout.height - 1);
  return format;
}

}  // namespace

MediaCodec::MediaCodec(CodecKind kind, std::string name)
    : kind_(kind), name_(std::move(name)) {}

MediaCodec::~MediaCodec() { Stop(); }

template <typename Predicate>
bool MediaCodec::WaitFor(std::unique_lock<std::mutex>& lock,
                         std::int64_t timeout_us, Predicate ready) {
  if (timeout_us < 0) {
    changed_.wait(lock, ready);
    return true;
  }
  return changed_.wait_for(lock, std::chrono::microseconds(timeout_us), ready);
}

media_status_t MediaCodec::Configure(const MediaFormat& format, bool surface,
                                     bool encoder) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (state_ != State::kUninitialized) {
    return AMEDIA_ERROR_INVALID_OPERATION;
  }
  if (surface || encoder) {
    return AMEDIA_ERROR_UNSUPPORTED;
  }
  decoder_ = FfmpegDecoder::Create(kind_, format);
  if (decoder_ == nullptr) {
    std::fprintf(stderr, "  [mediacodec] %s cannot open %s\n", name_.c_str(),
                 CodecMime(kind_));
    return AMEDIA_ERROR_UNSUPPORTED;
  }
  input_capacity_ = InputCapacity(kind_, format);
  OutputLayout initial;
  if (kind_ == CodecKind::kAac) {
    initial.sample_rate = Int32Or(format, "sample-rate", 44100);
    initial.channels = Int32Or(format, "channel-count", 2);
    std::fprintf(stderr, "  [mediacodec] %s: %s %d Hz, %d channels\n",
                 name_.c_str(), CodecMime(kind_), initial.sample_rate,
                 initial.channels);
  } else {
    initial.width = Int32Or(format, "width", 0);
    initial.height = Int32Or(format, "height", 0);
    initial.stride = initial.width;
    std::fprintf(stderr, "  [mediacodec] %s: %s %dx%d\n", name_.c_str(),
                 CodecMime(kind_), initial.width, initial.height);
  }
  output_format_ = OutputFormatFor(kind_, initial);
  state_ = State::kConfigured;
  return AMEDIA_OK;
}

media_status_t MediaCodec::Start() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (state_ != State::kConfigured) {
    return AMEDIA_ERROR_INVALID_OPERATION;
  }
  inputs_.assign(kInputSlots, Slot{});
  for (Slot& slot : inputs_) {
    slot.data.resize(input_capacity_);
  }
  outputs_.assign(kOutputSlots, Slot{});
  have_announced_ = false;
  ResetBuffersLocked();
  state_ = State::kRunning;
  worker_ = std::thread(&MediaCodec::Run, this);
  return AMEDIA_OK;
}

media_status_t MediaCodec::Stop() {
  std::unique_lock<std::mutex> lock(mutex_);
  if (state_ == State::kUninitialized) {
    return AMEDIA_OK;
  }
  if (stop_requested_) {
    changed_.wait(lock, [this] { return state_ == State::kUninitialized; });
    return AMEDIA_OK;
  }
  stop_requested_ = true;
  changed_.notify_all();
  lock.unlock();
  if (worker_.joinable()) {
    worker_.join();
  }
  lock.lock();
  decoder_.reset();
  inputs_.clear();
  outputs_.clear();
  free_inputs_.clear();
  pending_inputs_.clear();
  free_outputs_.clear();
  ready_.clear();
  have_layout_ = false;
  have_announced_ = false;
  input_ended_ = false;
  flush_requested_ = false;
  stop_requested_ = false;
  state_ = State::kUninitialized;
  changed_.notify_all();
  return AMEDIA_OK;
}

media_status_t MediaCodec::Flush() {
  std::unique_lock<std::mutex> lock(mutex_);
  if (state_ != State::kRunning) {
    return AMEDIA_ERROR_INVALID_OPERATION;
  }
  flush_requested_ = true;
  changed_.notify_all();
  changed_.wait(lock, [this] {
    return !flush_requested_ || state_ != State::kRunning;
  });
  return AMEDIA_OK;
}

ssize_t MediaCodec::DequeueInput(std::int64_t timeout_us) {
  std::unique_lock<std::mutex> lock(mutex_);
  if (state_ != State::kRunning) {
    return AMEDIA_ERROR_INVALID_OPERATION;
  }
  WaitFor(lock, timeout_us, [this] {
    return state_ != State::kRunning ||
           (!input_ended_ && !flush_requested_ && !free_inputs_.empty());
  });
  if (state_ != State::kRunning) {
    return AMEDIA_ERROR_INVALID_OPERATION;
  }
  if (input_ended_ || flush_requested_ || free_inputs_.empty()) {
    return AMEDIACODEC_INFO_TRY_AGAIN_LATER;
  }
  const std::size_t index = free_inputs_.front();
  free_inputs_.pop_front();
  inputs_[index].owner = Owner::kClient;
  return static_cast<ssize_t>(index);
}

std::uint8_t* MediaCodec::InputBuffer(std::size_t index, std::size_t* size) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (state_ != State::kRunning || index >= inputs_.size() ||
      inputs_[index].owner != Owner::kClient) {
    return nullptr;
  }
  if (size != nullptr) {
    *size = inputs_[index].data.size();
  }
  return inputs_[index].data.data();
}

media_status_t MediaCodec::QueueInput(std::size_t index, off_t offset,
                                      std::size_t size, std::uint64_t time_us,
                                      std::uint32_t flags) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (state_ != State::kRunning || input_ended_) {
    return AMEDIA_ERROR_INVALID_OPERATION;
  }
  if (index >= inputs_.size() || inputs_[index].owner != Owner::kClient) {
    return AMEDIA_ERROR_INVALID_PARAMETER;
  }
  Slot& slot = inputs_[index];
  if (offset < 0 || static_cast<std::size_t>(offset) > slot.data.size() ||
      size > slot.data.size() - static_cast<std::size_t>(offset)) {
    return AMEDIA_ERROR_INVALID_PARAMETER;
  }
  slot.info.offset = static_cast<std::int32_t>(offset);
  slot.info.size = static_cast<std::int32_t>(size);
  slot.info.presentationTimeUs = static_cast<std::int64_t>(time_us);
  slot.info.flags = flags;
  slot.owner = Owner::kPending;
  pending_inputs_.push_back(index);
  if ((flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) != 0) {
    input_ended_ = true;
  }
  changed_.notify_all();
  return AMEDIA_OK;
}

ssize_t MediaCodec::DequeueOutput(AMediaCodecBufferInfo* info,
                                  std::int64_t timeout_us) {
  std::unique_lock<std::mutex> lock(mutex_);
  if (state_ != State::kRunning) {
    return AMEDIA_ERROR_INVALID_OPERATION;
  }
  const bool ready = WaitFor(lock, timeout_us, [this] {
    return state_ != State::kRunning || (!flush_requested_ && !ready_.empty());
  });
  if (state_ != State::kRunning) {
    return AMEDIA_ERROR_INVALID_OPERATION;
  }
  if (!ready) {
    return AMEDIACODEC_INFO_TRY_AGAIN_LATER;
  }
  Ready next = std::move(ready_.front());
  ready_.pop_front();
  if (next.index < 0) {
    output_format_ = std::move(next.format);
    announced_layout_ = next.layout;
    have_announced_ = true;
    return AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED;
  }
  Slot& slot = outputs_[static_cast<std::size_t>(next.index)];
  slot.owner = Owner::kClient;
  if (info != nullptr) {
    *info = slot.info;
  }
  return next.index;
}

std::uint8_t* MediaCodec::OutputBuffer(std::size_t index, std::size_t* size) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (state_ != State::kRunning || index >= outputs_.size() ||
      outputs_[index].owner != Owner::kClient) {
    return nullptr;
  }
  if (size != nullptr) {
    *size = outputs_[index].data.size();
  }
  return outputs_[index].data.data();
}

media_status_t MediaCodec::ReleaseOutput(std::size_t index) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (state_ != State::kRunning) {
    return AMEDIA_ERROR_INVALID_OPERATION;
  }
  if (index >= outputs_.size() || outputs_[index].owner != Owner::kClient) {
    return AMEDIA_ERROR_INVALID_PARAMETER;
  }
  outputs_[index].owner = Owner::kCodec;
  free_outputs_.push_back(index);
  changed_.notify_all();
  return AMEDIA_OK;
}

MediaFormat MediaCodec::OutputFormat() {
  std::lock_guard<std::mutex> lock(mutex_);
  return output_format_;
}

bool MediaCodec::Interrupted() const {
  return stop_requested_ || flush_requested_;
}

void MediaCodec::ResetBuffersLocked() {
  free_inputs_.clear();
  pending_inputs_.clear();
  free_outputs_.clear();
  ready_.clear();
  for (std::size_t i = 0; i < inputs_.size(); ++i) {
    inputs_[i].owner = Owner::kCodec;
    free_inputs_.push_back(i);
  }
  for (std::size_t i = 0; i < outputs_.size(); ++i) {
    outputs_[i].owner = Owner::kCodec;
    free_outputs_.push_back(i);
  }
  input_ended_ = false;
  have_layout_ = have_announced_;
  layout_ = announced_layout_;
}

void MediaCodec::Run() {
  pthread_setname_np(pthread_self(), "MediaCodec");
  std::unique_lock<std::mutex> lock(mutex_);
  while (true) {
    changed_.wait(lock,
                  [this] { return Interrupted() || !pending_inputs_.empty(); });
    if (stop_requested_) {
      return;
    }
    if (flush_requested_) {
      lock.unlock();
      decoder_->Flush();
      lock.lock();
      ResetBuffersLocked();
      flush_requested_ = false;
      changed_.notify_all();
      continue;
    }
    const std::size_t index = pending_inputs_.front();
    pending_inputs_.pop_front();
    if (!Decode(lock, inputs_[index])) {
      continue;
    }
    inputs_[index].owner = Owner::kCodec;
    free_inputs_.push_back(index);
    changed_.notify_all();
  }
}

bool MediaCodec::Decode(std::unique_lock<std::mutex>& lock,
                        const Slot& input) {
  const AMediaCodecBufferInfo info = input.info;
  const std::uint8_t* data = input.data.data() + info.offset;
  const bool config = (info.flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG) != 0;
  const bool end = (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) != 0;
  lock.unlock();
  bool done = true;
  if (config && kind_ == CodecKind::kAac) {
    decoder_->SetConfig(data, static_cast<std::size_t>(info.size));
  } else if (info.size > 0) {
    while (decoder_->Send(data, static_cast<std::size_t>(info.size),
                          info.presentationTimeUs) ==
           FfmpegDecoder::Result::kAgain) {
      if (!ReceiveAll(lock, false)) {
        done = false;
        break;
      }
    }
    done = done && ReceiveAll(lock, false);
  }
  if (done && end) {
    decoder_->SendEnd();
    done = ReceiveAll(lock, true) && Deliver(lock, true);
  }
  lock.lock();
  return done;
}

bool MediaCodec::ReceiveAll(std::unique_lock<std::mutex>& lock,
                            bool until_end) {
  int errors = 0;
  while (true) {
    const FfmpegDecoder::Result received = decoder_->Receive();
    if (received == FfmpegDecoder::Result::kOk) {
      if (!Deliver(lock, false)) {
        return false;
      }
      continue;
    }
    if (received == FfmpegDecoder::Result::kError && until_end &&
        ++errors < 64) {
      continue;
    }
    return true;
  }
}

bool MediaCodec::Deliver(std::unique_lock<std::mutex>& lock,
                         bool end_of_stream) {
  OutputLayout layout;
  std::size_t size = 0;
  std::int64_t time_us = 0;
  if (!end_of_stream) {
    layout = decoder_->Layout();
    size = decoder_->OutputSize();
    time_us = decoder_->OutputTime();
  }
  lock.lock();
  changed_.wait(lock, [this] { return Interrupted() || !free_outputs_.empty(); });
  if (Interrupted()) {
    lock.unlock();
    return false;
  }
  const std::size_t index = free_outputs_.front();
  free_outputs_.pop_front();
  if (!end_of_stream && (!have_layout_ || !(layout == layout_))) {
    have_layout_ = true;
    layout_ = layout;
    ready_.push_back({AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED, layout,
                      OutputFormatFor(kind_, layout)});
  }
  Slot& slot = outputs_[index];
  lock.unlock();
  if (!end_of_stream) {
    if (slot.data.size() < size) {
      slot.data.resize(size);
    }
    decoder_->CopyOutput(slot.data.data());
  }
  lock.lock();
  slot.info.offset = 0;
  slot.info.size = static_cast<std::int32_t>(size);
  slot.info.presentationTimeUs = time_us;
  slot.info.flags = end_of_stream ? AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM : 0;
  ready_.push_back({static_cast<ssize_t>(index), OutputLayout{}, MediaFormat()});
  changed_.notify_all();
  lock.unlock();
  return true;
}

}  // namespace blox7::media

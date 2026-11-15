#ifndef BLOX7_MEDIA_MEDIA_CODEC_H_
#define BLOX7_MEDIA_MEDIA_CODEC_H_

#include <media/NdkMediaCodec.h>
#include <sys/types.h>

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "media/codec_names.h"
#include "media/encoding.h"
#include "media/ffmpeg_decoder.h"
#include "media/ffmpeg_encoder.h"
#include "media/h264_encoder.h"
#include "media/input_surface.h"
#include "media/media_format.h"

struct ANativeWindow;

namespace blox7::media {

class MediaCodec final : public FrameSink {
 public:
  MediaCodec(CodecKind kind, std::string name, bool encoder);
  ~MediaCodec();
  MediaCodec(const MediaCodec&) = delete;
  MediaCodec& operator=(const MediaCodec&) = delete;

  const std::string& name() const { return name_; }

  media_status_t Configure(const MediaFormat& format, bool surface,
                           bool encoder);
  media_status_t Start();
  media_status_t Stop();
  media_status_t Flush();
  ssize_t DequeueInput(std::int64_t timeout_us);
  std::uint8_t* InputBuffer(std::size_t index, std::size_t* size);
  media_status_t QueueInput(std::size_t index, off_t offset, std::size_t size,
                            std::uint64_t time_us, std::uint32_t flags);
  ssize_t DequeueOutput(AMediaCodecBufferInfo* info, std::int64_t timeout_us);
  std::uint8_t* OutputBuffer(std::size_t index, std::size_t* size);
  media_status_t ReleaseOutput(std::size_t index);
  MediaFormat OutputFormat();
  media_status_t CreateInputSurface(ANativeWindow** window);
  media_status_t SignalEndOfInputStream();

  void QueueFrame(std::int64_t time_us) override;
  void DropFrame(std::int64_t time_us) override;
  std::vector<std::uint8_t> TakeFrameBuffer(std::size_t size) override;
  void PushFrame(std::vector<std::uint8_t> frame,
                 std::int64_t time_us) override;

 private:
  enum class State { kUninitialized, kConfigured, kRunning };
  enum class Owner { kCodec, kClient, kPending };

  struct Slot {
    std::vector<std::uint8_t> data;
    Owner owner = Owner::kCodec;
    AMediaCodecBufferInfo info{};
  };

  struct Ready {
    ssize_t index = 0;
    OutputLayout layout;
    MediaFormat format;
  };

  struct QueuedFrame {
    std::vector<std::uint8_t> data;
    std::int64_t time_us = 0;
  };

  template <typename Predicate>
  bool WaitFor(std::unique_lock<std::mutex>& lock, std::int64_t timeout_us,
               Predicate ready);
  media_status_t ConfigureEncoderLocked(const MediaFormat& format);
  void Run();
  bool HasWork() const;
  bool InFlightBeforeEnd() const;
  void ForgetInFlightLocked(std::int64_t time_us);
  void FlushEngine();
  bool Decode(std::unique_lock<std::mutex>& lock, const Slot& input);
  bool Encode(std::unique_lock<std::mutex>& lock, const Slot& input);
  void EncodeFrame(std::unique_lock<std::mutex>& lock);
  bool DeliverPackets(std::unique_lock<std::mutex>& lock,
                      const std::vector<EncodedPacket>& packets);
  bool DeliverPacket(std::unique_lock<std::mutex>& lock,
                     const EncodedPacket& packet);
  MediaFormat EncoderOutputFormat() const;
  void RecycleLocked(std::vector<std::uint8_t> frame);
  bool ReceiveAll(std::unique_lock<std::mutex>& lock, bool until_end);
  bool Deliver(std::unique_lock<std::mutex>& lock, bool end_of_stream);
  bool Interrupted() const;
  void ResetBuffersLocked();

  const CodecKind kind_;
  const std::string name_;
  const bool encoder_;
  std::mutex mutex_;
  std::condition_variable changed_;
  State state_ = State::kUninitialized;
  MediaFormat output_format_;
  std::unique_ptr<FfmpegDecoder> decoder_;
  std::unique_ptr<H264Encoder> video_encoder_;
  std::unique_ptr<FfmpegEncoder> audio_encoder_;
  std::shared_ptr<InputSurface> surface_;
  bool surface_input_ = false;
  std::deque<QueuedFrame> frames_;
  std::vector<std::vector<std::uint8_t>> spare_frames_;
  std::deque<std::int64_t> in_flight_;
  bool surface_ended_ = false;
  std::int64_t end_time_us_ = 0;
  bool end_sent_ = false;
  bool have_frame_time_ = false;
  std::int64_t last_frame_time_us_ = 0;
  bool config_announced_ = false;
  bool drop_logged_ = false;
  bool failure_logged_ = false;
  std::size_t input_capacity_ = 0;
  std::vector<Slot> inputs_;
  std::vector<Slot> outputs_;
  std::deque<std::size_t> free_inputs_;
  std::deque<std::size_t> pending_inputs_;
  std::deque<std::size_t> free_outputs_;
  std::deque<Ready> ready_;
  bool have_layout_ = false;
  OutputLayout layout_;
  bool have_announced_ = false;
  OutputLayout announced_layout_;
  bool input_ended_ = false;
  bool flush_requested_ = false;
  bool stop_requested_ = false;
  std::thread worker_;
};

}  // namespace blox7::media

#endif  // BLOX7_MEDIA_MEDIA_CODEC_H_

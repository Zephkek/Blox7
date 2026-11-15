#include "media/h264_encoder.h"

#include <media/NdkMediaCodec.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <new>
#include <string>
#include <thread>

#include <wels/codec_api.h>

namespace blox7::media {
namespace {

constexpr std::int32_t kDefaultBitrate = 2000000;
constexpr float kDefaultFrameRate = 30.0f;
constexpr std::int32_t kColorRangeLimited = 2;
constexpr std::int32_t kColorStandardBt709 = 1;
constexpr std::int32_t kColorTransferSdrVideo = 3;
constexpr std::int32_t kDataspaceBt709Limited = 0x10C10000;
constexpr std::int32_t kBitrateModeVbr = 1;
constexpr std::int32_t kAvcProfileHigh = 8;

std::int32_t AvcLevel(int level_idc) {
  static constexpr struct {
    int idc;
    std::int32_t level;
  } kLevels[] = {{9, 0x2},      {10, 0x1},     {11, 0x4},     {12, 0x8},
                 {13, 0x10},    {20, 0x20},    {21, 0x40},    {22, 0x80},
                 {30, 0x100},   {31, 0x200},   {32, 0x400},   {40, 0x800},
                 {41, 0x1000},  {42, 0x2000},  {50, 0x4000},  {51, 0x8000},
                 {52, 0x10000}, {60, 0x20000}, {61, 0x40000}, {62, 0x80000}};
  for (const auto& entry : kLevels) {
    if (entry.idc == level_idc) {
      return entry.level;
    }
  }
  return 0;
}

std::int32_t Int32Or(const MediaFormat& format, const char* key,
                     std::int32_t fallback) {
  std::int32_t value = 0;
  return format.GetInt32(key, &value) ? value : fallback;
}

float FloatOr(const MediaFormat& format, const char* key, float fallback) {
  float value = 0;
  return format.GetFloat(key, &value) ? value : fallback;
}

unsigned int IntraPeriod(float seconds, float frame_rate) {
  if (seconds < 0) {
    return 0;
  }
  return static_cast<unsigned int>(
      std::max(1.0f, std::round(seconds * frame_rate)));
}

int ThreadCount(const PictureLayout& layout) {
  const int cores =
      static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
  const std::int64_t pixels =
      static_cast<std::int64_t>(layout.width) * layout.height;
  return static_cast<int>(
      std::clamp<std::int64_t>(pixels / (640 * 360), 1, std::min(cores, 4)));
}

std::size_t StartCodeLength(const std::uint8_t* nal, std::size_t size) {
  if (size >= 4 && nal[0] == 0 && nal[1] == 0 && nal[2] == 0 && nal[3] == 1) {
    return 4;
  }
  if (size >= 3 && nal[0] == 0 && nal[1] == 0 && nal[2] == 1) {
    return 3;
  }
  return 0;
}

}  // namespace

std::unique_ptr<H264Encoder> H264Encoder::Create(const MediaFormat& format) {
  const std::int32_t width = Int32Or(format, "width", 0);
  const std::int32_t height = Int32Or(format, "height", 0);
  const PictureLayout layout = PictureLayout::For(width, height);
  if (layout.width < 16 || layout.height < 16 || width > 4096 ||
      height > 4096) {
    std::fprintf(stderr, "  [mediacodec] no H.264 encoder for %dx%d\n", width,
                 height);
    return nullptr;
  }
  const std::int32_t bitrate =
      std::max(Int32Or(format, "bitrate", kDefaultBitrate), 16000);
  const float frame_rate =
      std::clamp(FloatOr(format, "frame-rate", kDefaultFrameRate), 1.0f,
                 240.0f);
  const float interval = FloatOr(format, "i-frame-interval", 1.0f);
  const int threads = ThreadCount(layout);

  ISVCEncoder* encoder = nullptr;
  if (WelsCreateSVCEncoder(&encoder) != 0 || encoder == nullptr) {
    return nullptr;
  }
  int trace = WELS_LOG_ERROR;
  encoder->SetOption(ENCODER_OPTION_TRACE_LEVEL, &trace);
  SEncParamExt params;
  encoder->GetDefaultParams(&params);
  params.iUsageType = CAMERA_VIDEO_REAL_TIME;
  params.iPicWidth = layout.width;
  params.iPicHeight = layout.height;
  params.iTargetBitrate = bitrate;
  params.iMaxBitrate = UNSPECIFIED_BIT_RATE;
  params.iRCMode = RC_BITRATE_MODE;
  params.fMaxFrameRate = frame_rate;
  params.iTemporalLayerNum = 1;
  params.iSpatialLayerNum = 1;
  params.uiIntraPeriod = IntraPeriod(interval, frame_rate);
  params.eSpsPpsIdStrategy = CONSTANT_ID;
  params.bPrefixNalAddingCtrl = false;
  params.bEnableFrameSkip = false;
  params.bEnableLongTermReference = false;
  params.bEnableDenoise = false;
  params.iEntropyCodingModeFlag = 1;
  params.iMultipleThreadIdc = threads;
  SSpatialLayerConfig& layer = params.sSpatialLayers[0];
  layer.iVideoWidth = layout.width;
  layer.iVideoHeight = layout.height;
  layer.fFrameRate = frame_rate;
  layer.iSpatialBitrate = bitrate;
  layer.iMaxSpatialBitrate = UNSPECIFIED_BIT_RATE;
  layer.uiProfileIdc = PRO_HIGH;
  layer.uiLevelIdc = LEVEL_UNKNOWN;
  layer.sSliceArgument.uiSliceMode =
      threads > 1 ? SM_FIXEDSLCNUM_SLICE : SM_SINGLE_SLICE;
  layer.sSliceArgument.uiSliceNum = static_cast<unsigned int>(threads);
  layer.bVideoSignalTypePresent = true;
  layer.uiVideoFormat = VF_UNDEF;
  layer.bFullRange = false;
  layer.bColorDescriptionPresent = true;
  layer.uiColorPrimaries = CP_BT709;
  layer.uiTransferCharacteristics = TRC_BT709;
  layer.uiColorMatrix = CM_BT709;
  if (encoder->InitializeExt(&params) != cmResultSuccess) {
    std::fprintf(stderr, "  [mediacodec] OpenH264 refused %dx%d at %d bps\n",
                 layout.width, layout.height, bitrate);
    WelsDestroySVCEncoder(encoder);
    return nullptr;
  }
  int data_format = videoFormatI420;
  encoder->SetOption(ENCODER_OPTION_DATAFORMAT, &data_format);
  if (layout.width != width || layout.height != height) {
    std::fprintf(stderr, "  [mediacodec] encoding %dx%d as %dx%d\n", width,
                 height, layout.width, layout.height);
  }
  std::unique_ptr<H264Encoder> created(new (std::nothrow) H264Encoder(
      encoder, width, height, bitrate, frame_rate));
  if (created == nullptr) {
    WelsDestroySVCEncoder(encoder);
  }
  return created;
}

H264Encoder::H264Encoder(ISVCEncoder* encoder, std::int32_t width,
                         std::int32_t height, std::int32_t bitrate,
                         float frame_rate)
    : encoder_(encoder),
      width_(width),
      height_(height),
      layout_(PictureLayout::For(width, height)),
      bitrate_(bitrate),
      frame_rate_(frame_rate) {}

H264Encoder::~H264Encoder() {
  encoder_->Uninitialize();
  WelsDestroySVCEncoder(encoder_);
}

bool H264Encoder::Encode(const std::uint8_t* picture, std::int64_t time_us,
                         std::vector<EncodedPacket>* out) {
  SSourcePicture source{};
  source.iColorFormat = videoFormatI420;
  source.iPicWidth = layout_.width;
  source.iPicHeight = layout_.height;
  source.iStride[0] = layout_.luma_stride;
  source.iStride[1] = layout_.chroma_stride;
  source.iStride[2] = layout_.chroma_stride;
  auto* base = const_cast<std::uint8_t*>(picture);
  source.pData[0] = base;
  source.pData[1] = base + layout_.LumaSize();
  source.pData[2] = base + layout_.LumaSize() + layout_.ChromaSize();
  source.uiTimeStamp = time_us / 1000;
  SFrameBSInfo info{};
  if (encoder_->EncodeFrame(&source, &info) != cmResultSuccess) {
    return false;
  }
  if (info.eFrameType == videoFrameTypeSkip ||
      info.eFrameType == videoFrameTypeInvalid) {
    return true;
  }
  EncodedPacket frame;
  frame.time_us = time_us;
  frame.flags =
      info.eFrameType == videoFrameTypeIDR ? AMEDIACODEC_BUFFER_FLAG_KEY_FRAME : 0;
  for (int layer = 0; layer < info.iLayerNum; ++layer) {
    const SLayerBSInfo& bits = info.sLayerInfo[layer];
    const std::uint8_t* nal = bits.pBsBuf;
    for (int index = 0; index < bits.iNalCount; ++index) {
      const std::size_t size = static_cast<std::size_t>(bits.pNalLengthInByte[index]);
      const std::size_t start = StartCodeLength(nal, size);
      const int type = start < size ? nal[start] & 0x1f : 0;
      if (type == 7 || type == 8) {
        std::vector<std::uint8_t>& kept = type == 7 ? sps_ : pps_;
        if (!config_sent_) {
          kept.assign(nal, nal + size);
        }
      } else {
        frame.data.insert(frame.data.end(), nal, nal + size);
      }
      nal += size;
    }
  }
  if (!config_sent_ && !sps_.empty() && !pps_.empty()) {
    EncodedPacket config;
    config.data = sps_;
    config.data.insert(config.data.end(), pps_.begin(), pps_.end());
    config.time_us = time_us;
    config.flags = AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG;
    out->push_back(std::move(config));
    config_sent_ = true;
  }
  if (!frame.data.empty()) {
    out->push_back(std::move(frame));
  }
  return true;
}

void H264Encoder::RequestKeyFrame() { encoder_->ForceIntraFrame(true); }

MediaFormat H264Encoder::OutputFormat() const {
  MediaFormat format;
  format.Set("bitrate", bitrate_);
  format.Set("bitrate-mode", kBitrateModeVbr);
  format.Set("width", layout_.width);
  format.Set("color-standard", kColorStandardBt709);
  format.Set("color-range", kColorRangeLimited);
  format.Set("color-transfer", kColorTransferSdrVideo);
  format.Set("frame-rate", static_cast<std::int32_t>(std::lround(frame_rate_)));
  format.Set("height", layout_.height);
  format.Set("intra-refresh-period", 0);
  if (sps_.size() > 7) {
    format.Set("level", AvcLevel(sps_[7]));
  }
  format.Set("max-bitrate", bitrate_);
  format.Set("mime", std::string("video/avc"));
  format.Set("prepend-sps-pps-to-idr-frames", 0);
  format.Set("priority", 0);
  format.Set("profile", kAvcProfileHigh);
  if (!sps_.empty() && !pps_.empty()) {
    format.Set("csd-0", sps_);
    format.Set("android._dataspace", kDataspaceBt709Limited);
    format.Set("csd-1", pps_);
  }
  return format;
}

}  // namespace blox7::media

#include "media/h264_encoder.h"

#include <media/NdkMediaCodec.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

std::int32_t AvcProfile(int profile_idc, int constraints) {
  switch (profile_idc) {
    case 66:
      return (constraints & 0x40) != 0 ? 0x10000 : 0x1;
    case 77:
      return 0x2;
    case 88:
      return 0x4;
    case 100:
      return (constraints & 0x0C) == 0x0C ? 0x80000 : kAvcProfileHigh;
    case 110:
      return 0x10;
    case 122:
      return 0x20;
    case 244:
      return 0x40;
    default:
      return kAvcProfileHigh;
  }
}

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

bool ReadAvcSettings(const MediaFormat& format, AvcSettings* settings) {
  settings->width = format.Int32Or("width", 0);
  settings->height = format.Int32Or("height", 0);
  const PictureLayout layout =
      PictureLayout::For(settings->width, settings->height);
  if (layout.width < 16 || layout.height < 16 || settings->width > 4096 ||
      settings->height > 4096) {
    std::fprintf(stderr, "  [mediacodec] no H.264 encoder for %dx%d\n",
                 settings->width, settings->height);
    return false;
  }
  settings->bitrate =
      std::max(format.Int32Or("bitrate", kDefaultBitrate), 16000);
  settings->frame_rate =
      std::clamp(format.FloatOr("frame-rate", kDefaultFrameRate), 1.0f,
                 240.0f);
  settings->key_frame_interval = format.FloatOr("i-frame-interval", 1.0f);
  if (layout.width != settings->width || layout.height != settings->height) {
    std::fprintf(stderr, "  [mediacodec] encoding %dx%d as %dx%d\n",
                 settings->width, settings->height, layout.width,
                 layout.height);
  }
  return true;
}

}  // namespace

std::unique_ptr<VideoEncoder> CreateAvcEncoder(const MediaFormat& format) {
  AvcSettings settings;
  if (!ReadAvcSettings(format, &settings)) {
    return nullptr;
  }
  const char* choice = std::getenv("BLOX7_VIDEO_ENCODER");
  if (choice == nullptr || std::strcmp(choice, "software") != 0) {
    if (std::unique_ptr<VideoEncoder> hardware =
            CreateHardwareAvcEncoder(settings)) {
      return hardware;
    }
  }
  return H264Encoder::Create(settings);
}

MediaFormat AvcOutputFormat(const AvcSettings& settings,
                            const PictureLayout& layout,
                            const std::vector<std::uint8_t>& sps,
                            const std::vector<std::uint8_t>& pps) {
  const std::size_t start = StartCodeLength(sps.data(), sps.size());
  const bool parsed = start != 0 && sps.size() >= start + 4;
  MediaFormat format;
  format.Set("bitrate", settings.bitrate);
  format.Set("bitrate-mode", kBitrateModeVbr);
  format.Set("width", layout.width);
  format.Set("color-standard", kColorStandardBt709);
  format.Set("color-range", kColorRangeLimited);
  format.Set("color-transfer", kColorTransferSdrVideo);
  format.Set("frame-rate",
             static_cast<std::int32_t>(std::lround(settings.frame_rate)));
  format.Set("height", layout.height);
  format.Set("intra-refresh-period", 0);
  if (parsed) {
    format.Set("level", AvcLevel(sps[start + 3]));
  }
  format.Set("max-bitrate", settings.bitrate);
  format.Set("mime", std::string("video/avc"));
  format.Set("prepend-sps-pps-to-idr-frames", 0);
  format.Set("priority", 0);
  format.Set("profile", parsed ? AvcProfile(sps[start + 1], sps[start + 2])
                               : kAvcProfileHigh);
  if (!sps.empty() && !pps.empty()) {
    format.Set("csd-0", sps);
    format.Set("android._dataspace", kDataspaceBt709Limited);
    format.Set("csd-1", pps);
  }
  return format;
}

std::unique_ptr<H264Encoder> H264Encoder::Create(const AvcSettings& settings) {
  const PictureLayout layout =
      PictureLayout::For(settings.width, settings.height);
  const std::int32_t bitrate = settings.bitrate;
  const float frame_rate = settings.frame_rate;
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
  params.uiIntraPeriod = IntraPeriod(settings.key_frame_interval, frame_rate);
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
  std::unique_ptr<H264Encoder> created(
      new (std::nothrow) H264Encoder(encoder, settings));
  if (created == nullptr) {
    WelsDestroySVCEncoder(encoder);
  }
  return created;
}

H264Encoder::H264Encoder(ISVCEncoder* encoder, const AvcSettings& settings)
    : encoder_(encoder),
      settings_(settings),
      layout_(PictureLayout::For(settings.width, settings.height)) {}

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
  frame.flags = info.eFrameType == videoFrameTypeIDR
                    ? AMEDIACODEC_BUFFER_FLAG_KEY_FRAME
                    : 0;
  for (int layer = 0; layer < info.iLayerNum; ++layer) {
    const SLayerBSInfo& bits = info.sLayerInfo[layer];
    const std::uint8_t* nal = bits.pBsBuf;
    for (int index = 0; index < bits.iNalCount; ++index) {
      const std::size_t size =
          static_cast<std::size_t>(bits.pNalLengthInByte[index]);
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

bool H264Encoder::Drain(std::vector<EncodedPacket>*) { return true; }

void H264Encoder::RequestKeyFrame() { encoder_->ForceIntraFrame(true); }

MediaFormat H264Encoder::OutputFormat() const {
  return AvcOutputFormat(settings_, layout_, sps_, pps_);
}

}  // namespace blox7::media

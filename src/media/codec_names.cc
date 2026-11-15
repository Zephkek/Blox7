#include "media/codec_names.h"

#include <strings.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>

namespace blox7::media {
namespace {

struct Family {
  const char* markers[4];
  const char* avc;
  const char* hevc;
  const char* avc_encoder;
};

const Family kFamilies[] = {
    {{"snapdragon", "qualcomm", "qcom", nullptr},
     "c2.qti.avc.decoder",
     "c2.qti.hevc.decoder",
     "c2.qti.avc.encoder"},
    {{"mediatek", "helio", "dimensity", nullptr},
     "c2.mtk.avc.decoder",
     "c2.mtk.hevc.decoder",
     "c2.mtk.avc.encoder"},
    {{nullptr},
     "c2.exynos.h264.decoder",
     "c2.exynos.hevc.decoder",
     "c2.exynos.h264.encoder"},
};

constexpr const char* kAacName = "c2.android.aac.decoder";
constexpr const char* kAacEncoderName = "c2.android.aac.encoder";

const Family& DeviceFamily() {
  const char* value = std::getenv("BLOX7_DEVICE_SOC_MODEL");
  std::string soc = value != nullptr ? value : "";
  std::transform(soc.begin(), soc.end(), soc.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  for (const Family& family : kFamilies) {
    for (const char* marker : family.markers) {
      if (marker != nullptr && soc.find(marker) != std::string::npos) {
        return family;
      }
    }
  }
  return kFamilies[2];
}

}  // namespace

bool KindForMime(const char* mime, CodecKind* kind) {
  if (strcasecmp(mime, "video/avc") == 0) {
    *kind = CodecKind::kAvc;
  } else if (strcasecmp(mime, "video/hevc") == 0) {
    *kind = CodecKind::kHevc;
  } else if (strcasecmp(mime, "audio/mp4a-latm") == 0) {
    *kind = CodecKind::kAac;
  } else {
    return false;
  }
  return true;
}

bool KindForName(const char* name, CodecKind* kind, bool* encoder) {
  *encoder = false;
  for (const Family& family : kFamilies) {
    if (std::strcmp(name, family.avc) == 0) {
      *kind = CodecKind::kAvc;
      return true;
    }
    if (std::strcmp(name, family.hevc) == 0) {
      *kind = CodecKind::kHevc;
      return true;
    }
    if (std::strcmp(name, family.avc_encoder) == 0) {
      *kind = CodecKind::kAvc;
      *encoder = true;
      return true;
    }
  }
  if (std::strcmp(name, kAacName) == 0) {
    *kind = CodecKind::kAac;
    return true;
  }
  if (std::strcmp(name, kAacEncoderName) == 0) {
    *kind = CodecKind::kAac;
    *encoder = true;
    return true;
  }
  return false;
}

const char* CodecMime(CodecKind kind) {
  switch (kind) {
    case CodecKind::kAvc:
      return "video/avc";
    case CodecKind::kHevc:
      return "video/hevc";
    case CodecKind::kAac:
      return "audio/mp4a-latm";
  }
  return "";
}

std::string CodecName(CodecKind kind) {
  switch (kind) {
    case CodecKind::kAvc:
      return DeviceFamily().avc;
    case CodecKind::kHevc:
      return DeviceFamily().hevc;
    case CodecKind::kAac:
      return kAacName;
  }
  return "";
}

std::string EncoderName(CodecKind kind) {
  switch (kind) {
    case CodecKind::kAvc:
      return DeviceFamily().avc_encoder;
    case CodecKind::kHevc:
      return "";
    case CodecKind::kAac:
      return kAacEncoderName;
  }
  return "";
}

}  // namespace blox7::media

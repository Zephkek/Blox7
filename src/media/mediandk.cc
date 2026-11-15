#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>

#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <utility>

#include "media/codec_names.h"
#include "media/media_codec.h"
#include "media/media_format.h"

using blox7::media::CodecKind;

struct AMediaFormat {
  blox7::media::MediaFormat format;
  std::string text;
};

struct AMediaCodec {
  AMediaCodec(CodecKind kind, std::string name, bool encoder)
      : codec(kind, std::move(name), encoder) {}
  blox7::media::MediaCodec codec;
};

#pragma GCC visibility push(default)
extern "C" {

const char* AMEDIAFORMAT_KEY_MIME = "mime";
const char* AMEDIAFORMAT_KEY_WIDTH = "width";
const char* AMEDIAFORMAT_KEY_HEIGHT = "height";
const char* AMEDIAFORMAT_KEY_BIT_RATE = "bitrate";
const char* AMEDIAFORMAT_KEY_FRAME_RATE = "frame-rate";
const char* AMEDIAFORMAT_KEY_COLOR_FORMAT = "color-format";
const char* AMEDIAFORMAT_KEY_CHANNEL_COUNT = "channel-count";
const char* AMEDIAFORMAT_KEY_SAMPLE_RATE = "sample-rate";
const char* AMEDIAFORMAT_KEY_I_FRAME_INTERVAL = "i-frame-interval";
const char* AMEDIAFORMAT_KEY_STRIDE = "stride";

AMediaFormat* AMediaFormat_new() { return new (std::nothrow) AMediaFormat; }

media_status_t AMediaFormat_delete(AMediaFormat* format) {
  delete format;
  return AMEDIA_OK;
}

const char* AMediaFormat_toString(AMediaFormat* format) {
  if (format == nullptr) {
    return "";
  }
  format->text = format->format.ToString();
  return format->text.c_str();
}

bool AMediaFormat_getInt32(AMediaFormat* format, const char* name,
                           int32_t* out) {
  return format != nullptr && name != nullptr && out != nullptr &&
         format->format.GetInt32(name, out);
}

bool AMediaFormat_getBuffer(AMediaFormat* format, const char* name,
                            void** data, size_t* size) {
  if (format == nullptr || name == nullptr || data == nullptr ||
      size == nullptr) {
    return false;
  }
  const blox7::media::MediaFormat::Buffer* buffer =
      format->format.GetBuffer(name);
  if (buffer == nullptr) {
    return false;
  }
  *data = const_cast<std::uint8_t*>(buffer->data());
  *size = buffer->size();
  return true;
}

void AMediaFormat_setInt32(AMediaFormat* format, const char* name,
                           int32_t value) {
  if (format != nullptr && name != nullptr) {
    format->format.Set(name, value);
  }
}

void AMediaFormat_setFloat(AMediaFormat* format, const char* name,
                           float value) {
  if (format != nullptr && name != nullptr) {
    format->format.Set(name, value);
  }
}

void AMediaFormat_setString(AMediaFormat* format, const char* name,
                            const char* value) {
  if (format != nullptr && name != nullptr && value != nullptr) {
    format->format.Set(name, std::string(value));
  }
}

void AMediaFormat_setBuffer(AMediaFormat* format, const char* name,
                            const void* data, size_t size) {
  if (format == nullptr || name == nullptr || (data == nullptr && size != 0)) {
    return;
  }
  const auto* bytes = static_cast<const std::uint8_t*>(data);
  format->format.Set(name,
                     blox7::media::MediaFormat::Buffer(bytes, bytes + size));
}

AMediaCodec* AMediaCodec_createCodecByName(const char* name) {
  CodecKind kind;
  bool encoder = false;
  if (name == nullptr || !blox7::media::KindForName(name, &kind, &encoder)) {
    return nullptr;
  }
  return new (std::nothrow) AMediaCodec(kind, name, encoder);
}

AMediaCodec* AMediaCodec_createDecoderByType(const char* mime_type) {
  CodecKind kind;
  if (mime_type == nullptr || !blox7::media::KindForMime(mime_type, &kind)) {
    return nullptr;
  }
  return new (std::nothrow)
      AMediaCodec(kind, blox7::media::CodecName(kind), false);
}

AMediaCodec* AMediaCodec_createEncoderByType(const char* mime_type) {
  CodecKind kind;
  if (mime_type == nullptr || !blox7::media::KindForMime(mime_type, &kind) ||
      kind == CodecKind::kHevc) {
    return nullptr;
  }
  return new (std::nothrow)
      AMediaCodec(kind, blox7::media::EncoderName(kind), true);
}

media_status_t AMediaCodec_delete(AMediaCodec* codec) {
  delete codec;
  return AMEDIA_OK;
}

media_status_t AMediaCodec_configure(AMediaCodec* codec,
                                     const AMediaFormat* format,
                                     ANativeWindow* surface,
                                     AMediaCrypto* crypto, uint32_t flags) {
  if (codec == nullptr || format == nullptr) {
    return AMEDIA_ERROR_INVALID_PARAMETER;
  }
  return codec->codec.Configure(
      format->format, surface != nullptr || crypto != nullptr,
      (flags & AMEDIACODEC_CONFIGURE_FLAG_ENCODE) != 0);
}

media_status_t AMediaCodec_start(AMediaCodec* codec) {
  return codec != nullptr ? codec->codec.Start()
                          : AMEDIA_ERROR_INVALID_OBJECT;
}

media_status_t AMediaCodec_stop(AMediaCodec* codec) {
  return codec != nullptr ? codec->codec.Stop() : AMEDIA_ERROR_INVALID_OBJECT;
}

media_status_t AMediaCodec_flush(AMediaCodec* codec) {
  return codec != nullptr ? codec->codec.Flush()
                          : AMEDIA_ERROR_INVALID_OBJECT;
}

media_status_t AMediaCodec_createInputSurface(AMediaCodec* codec,
                                              ANativeWindow** surface) {
  if (codec == nullptr || surface == nullptr) {
    return AMEDIA_ERROR_INVALID_PARAMETER;
  }
  return codec->codec.CreateInputSurface(surface);
}

media_status_t AMediaCodec_signalEndOfInputStream(AMediaCodec* codec) {
  return codec != nullptr ? codec->codec.SignalEndOfInputStream()
                          : AMEDIA_ERROR_INVALID_OBJECT;
}

uint8_t* AMediaCodec_getInputBuffer(AMediaCodec* codec, size_t idx,
                                    size_t* out_size) {
  return codec != nullptr ? codec->codec.InputBuffer(idx, out_size) : nullptr;
}

uint8_t* AMediaCodec_getOutputBuffer(AMediaCodec* codec, size_t idx,
                                     size_t* out_size) {
  return codec != nullptr ? codec->codec.OutputBuffer(idx, out_size) : nullptr;
}

ssize_t AMediaCodec_dequeueInputBuffer(AMediaCodec* codec, int64_t timeoutUs) {
  return codec != nullptr ? codec->codec.DequeueInput(timeoutUs)
                          : AMEDIA_ERROR_INVALID_OBJECT;
}

media_status_t AMediaCodec_queueInputBuffer(AMediaCodec* codec, size_t idx,
                                            off_t offset, size_t size,
                                            uint64_t time, uint32_t flags) {
  return codec != nullptr
             ? codec->codec.QueueInput(idx, offset, size, time, flags)
             : AMEDIA_ERROR_INVALID_OBJECT;
}

ssize_t AMediaCodec_dequeueOutputBuffer(AMediaCodec* codec,
                                        AMediaCodecBufferInfo* info,
                                        int64_t timeoutUs) {
  return codec != nullptr ? codec->codec.DequeueOutput(info, timeoutUs)
                          : AMEDIA_ERROR_INVALID_OBJECT;
}

AMediaFormat* AMediaCodec_getOutputFormat(AMediaCodec* codec) {
  if (codec == nullptr) {
    return nullptr;
  }
  AMediaFormat* format = new (std::nothrow) AMediaFormat;
  if (format != nullptr) {
    format->format = codec->codec.OutputFormat();
  }
  return format;
}

media_status_t AMediaCodec_releaseOutputBuffer(AMediaCodec* codec, size_t idx,
                                               bool) {
  return codec != nullptr ? codec->codec.ReleaseOutput(idx)
                          : AMEDIA_ERROR_INVALID_OBJECT;
}

media_status_t AMediaCodec_getName(AMediaCodec* codec, char** out_name) {
  if (codec == nullptr || out_name == nullptr) {
    return AMEDIA_ERROR_INVALID_PARAMETER;
  }
  *out_name = strdup(codec->codec.name().c_str());
  return *out_name != nullptr ? AMEDIA_OK : AMEDIA_ERROR_UNKNOWN;
}

void AMediaCodec_releaseName(AMediaCodec*, char* name) { std::free(name); }

}  // extern "C"
#pragma GCC visibility pop

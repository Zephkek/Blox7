#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cstdio>

struct AMediaCodec {};
struct AMediaFormat {
  char mime[64];
};
struct AImageReader;
struct AImage;
struct AHardwareBuffer;
struct ANativeWindow;

using media_status_t = int32_t;
static constexpr media_status_t AMEDIA_OK = 0;
static constexpr media_status_t AMEDIA_ERROR_UNKNOWN = -1;

extern "C" {

const char* AMEDIAFORMAT_KEY_MIME          = "mime";
const char* AMEDIAFORMAT_KEY_WIDTH         = "width";
const char* AMEDIAFORMAT_KEY_HEIGHT        = "height";
const char* AMEDIAFORMAT_KEY_BIT_RATE      = "bitrate";
const char* AMEDIAFORMAT_KEY_FRAME_RATE    = "frame-rate";
const char* AMEDIAFORMAT_KEY_COLOR_FORMAT  = "color-format";
const char* AMEDIAFORMAT_KEY_CHANNEL_COUNT = "channel-count";
const char* AMEDIAFORMAT_KEY_SAMPLE_RATE   = "sample-rate";
const char* AMEDIAFORMAT_KEY_I_FRAME_INTERVAL = "i-frame-interval";
const char* AMEDIAFORMAT_KEY_STRIDE        = "stride";

AMediaFormat* AMediaFormat_new() {
  return static_cast<AMediaFormat*>(calloc(1, sizeof(AMediaFormat)));
}
void AMediaFormat_delete(AMediaFormat* fmt) { free(fmt); }
bool AMediaFormat_getInt32(AMediaFormat*, const char*, int32_t* v) {
  if (v) *v = 0;
  return false;
}
bool AMediaFormat_getBuffer(AMediaFormat*, const char*,
                             void**, size_t*) { return false; }
void AMediaFormat_setInt32(AMediaFormat*, const char*,
                            int32_t) {}
void AMediaFormat_setFloat(AMediaFormat*, const char*,
                            float) {}
void AMediaFormat_setString(AMediaFormat*, const char* k,
                             const char* v) {
  (void)k; (void)v;
}
void AMediaFormat_setBuffer(AMediaFormat*, const char*,
                             const void*, size_t) {}
const char* AMediaFormat_toString(AMediaFormat*) { return "{}"; }

AMediaCodec* AMediaCodec_createDecoderByType(const char*) {
  return nullptr;
}
AMediaCodec* AMediaCodec_createEncoderByType(const char*) {
  return nullptr;
}
media_status_t AMediaCodec_delete(AMediaCodec*) {
  return AMEDIA_OK;
}
media_status_t AMediaCodec_configure(AMediaCodec*,
                                      const AMediaFormat*,
                                      void*, void*,
                                      uint32_t) {
  return AMEDIA_ERROR_UNKNOWN;
}
media_status_t AMediaCodec_start(AMediaCodec*) {
  return AMEDIA_ERROR_UNKNOWN;
}
media_status_t AMediaCodec_stop(AMediaCodec*) { return AMEDIA_OK; }
media_status_t AMediaCodec_flush(AMediaCodec*) { return AMEDIA_OK; }
uint8_t* AMediaCodec_getInputBuffer(AMediaCodec*, size_t,
                                     size_t*) { return nullptr; }
uint8_t* AMediaCodec_getOutputBuffer(AMediaCodec*, size_t,
                                      size_t*) { return nullptr; }
ssize_t AMediaCodec_dequeueInputBuffer(AMediaCodec*,
                                        int64_t) { return -1; }
ssize_t AMediaCodec_dequeueOutputBuffer(AMediaCodec*,
                                         void*,
                                         int64_t) { return -1; }
media_status_t AMediaCodec_queueInputBuffer(AMediaCodec*,
                                             size_t, size_t,
                                             size_t, uint64_t,
                                             uint32_t) {
  return AMEDIA_ERROR_UNKNOWN;
}
media_status_t AMediaCodec_releaseOutputBuffer(AMediaCodec*,
                                                size_t,
                                                bool) {
  return AMEDIA_OK;
}
AMediaFormat* AMediaCodec_getOutputFormat(AMediaCodec*) {
  return AMediaFormat_new();
}

// no video decoders here, so no image readers either
static constexpr media_status_t AMEDIA_ERROR_UNSUPPORTED = -10002;
static constexpr media_status_t AMEDIA_ERROR_INVALID_PARAMETER = -10004;

media_status_t AImageReader_newWithUsage(int32_t, int32_t, int32_t, uint64_t,
                                         int32_t, AImageReader** reader) {
  if (reader) *reader = nullptr;
  return AMEDIA_ERROR_UNSUPPORTED;
}
void AImageReader_delete(AImageReader*) {}
media_status_t AImageReader_getWindow(AImageReader*, ANativeWindow** window) {
  if (window) *window = nullptr;
  return AMEDIA_ERROR_INVALID_PARAMETER;
}
media_status_t AImageReader_acquireNextImage(AImageReader*, AImage** image) {
  if (image) *image = nullptr;
  return AMEDIA_ERROR_INVALID_PARAMETER;
}
void AImage_delete(AImage*) {}
media_status_t AImage_getHardwareBuffer(const AImage*,
                                        AHardwareBuffer** buffer) {
  if (buffer) *buffer = nullptr;
  return AMEDIA_ERROR_INVALID_PARAMETER;
}
media_status_t AImage_getTimestamp(const AImage*, int64_t* timestamp) {
  if (timestamp) *timestamp = 0;
  return AMEDIA_ERROR_INVALID_PARAMETER;
}

}  // extern "C"

#include <cstdint>

struct AImageReader;
struct AImage;
struct AHardwareBuffer;
struct ANativeWindow;

using media_status_t = int32_t;

#pragma GCC visibility push(default)
extern "C" {

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
#pragma GCC visibility pop

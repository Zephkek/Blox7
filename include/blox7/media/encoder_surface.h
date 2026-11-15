#ifndef BLOX7_MEDIA_ENCODER_SURFACE_H_
#define BLOX7_MEDIA_ENCODER_SURFACE_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BLOX7_ENCODER_SURFACE_SYMBOL "blox7_media_encoder_surface"

enum {
  BLOX7_ENCODER_PIXELS_RGBA = 0,
  BLOX7_ENCODER_PIXELS_BGRA = 1,
};

typedef struct Blox7EncoderSurface {
  void (*size)(const void* window, int32_t* width, int32_t* height);
  void (*acquire)(const void* window);
  void (*release)(const void* window);
  void (*queue)(const void* window, int64_t time_us);
  void (*submit)(const void* window, const void* pixels, int32_t stride,
                 int32_t pixels_format, int64_t time_us);
} Blox7EncoderSurface;

typedef const Blox7EncoderSurface* (*Blox7EncoderSurfaceLookup)(
    const void* window);

#ifdef __cplusplus
}
#endif

#endif  // BLOX7_MEDIA_ENCODER_SURFACE_H_

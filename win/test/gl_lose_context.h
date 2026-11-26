#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define LOSEC_GL(X)                                                                                              \
  X(unsigned int, glGetError, (void))                                                                            \
  X(const unsigned char*, glGetString, (unsigned int))                                                           \
  X(void, glGenTextures, (int, unsigned int*))                                                                   \
  X(void, glDeleteTextures, (int, const unsigned int*))                                                          \
  X(void, glBindTexture, (unsigned int, unsigned int))                                                           \
  X(void, glTexParameteri, (unsigned int, unsigned int, int))                                                    \
  X(void, glTexStorage2D, (unsigned int, int, unsigned int, int, int))                                           \
  X(void, glTexSubImage2D, (unsigned int, int, int, int, int, int, unsigned int, unsigned int, const void*))     \
  X(void, glCompressedTexSubImage2D, (unsigned int, int, int, int, int, int, unsigned int, int, const void*))    \
  X(void, glGenFramebuffers, (int, unsigned int*))                                                               \
  X(void, glBindFramebuffer, (unsigned int, unsigned int))                                                       \
  X(void, glFramebufferTexture2D, (unsigned int, unsigned int, unsigned int, unsigned int, int))                 \
  X(void, glGenVertexArrays, (int, unsigned int*))                                                               \
  X(void, glBindVertexArray, (unsigned int))                                                                     \
  X(unsigned int, glCreateProgram, (void))                                                                       \
  X(unsigned int, glCreateShader, (unsigned int))                                                                \
  X(void, glShaderSource, (unsigned int, int, const char* const*, const int*))                                   \
  X(void, glCompileShader, (unsigned int))                                                                       \
  X(void, glAttachShader, (unsigned int, unsigned int))                                                          \
  X(void, glLinkProgram, (unsigned int))                                                                         \
  X(void, glUseProgram, (unsigned int))                                                                          \
  X(int, glGetUniformLocation, (unsigned int, const char*))                                                      \
  X(void, glUniform1i, (int, int))                                                                               \
  X(void, glViewport, (int, int, int, int))                                                                      \
  X(void, glDisable, (unsigned int))                                                                             \
  X(void, glDrawArrays, (unsigned int, int, int))                                                                \
  X(void, glReadPixels, (int, int, int, int, unsigned int, unsigned int, void*))                                 \
  X(void, glPixelStorei, (unsigned int, int))                                                                    \
  X(void, glFinish, (void))

#define LOSEC_DECLARE(ret, name, args) static ret (BRIDGE_MS* losec_##name) args;
LOSEC_GL(LOSEC_DECLARE)

static int g_losec_read, g_losec_times, g_losec_done, g_losec_lost, g_losec_failed_swaps, g_losec_mid_frame;
static double g_losec_after, g_losec_gap, g_losec_next, g_losec_at;
static unsigned long long g_losec_swaps, g_losec_frames;
static unsigned int g_losec_program, g_losec_vao, g_losec_fbo, g_losec_target;

static double losec_now(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

static void losec_config(void) {
  if (g_losec_read) return;
  g_losec_read = 1;
  const char* text = getenv("BLOX7_GL_LOSE_CONTEXT");
  if (text == NULL || text[0] == '\0') return;
  char* end = NULL;
  g_losec_after = strtod(text, &end);
  g_losec_times = 1;
  g_losec_gap = 8;
  if (end && *end == ',') g_losec_times = (int)strtol(end + 1, &end, 10);
  if (end && *end == ',') g_losec_gap = strtod(end + 1, &end);
  if (end && *end == ',') g_losec_mid_frame = end[1] == 'm';
  fprintf(stderr, "[lose context] after %.1f s, %d times, %.1f s apart, %s\n", g_losec_after, g_losec_times, g_losec_gap,
          g_losec_mid_frame ? "right after a swap so the next frame's calls meet it" : "right before a swap");
}

static void losec_fetch(unsigned int texture, int w, int h) {
  if (g_losec_target) losec_glDeleteTextures(1, &g_losec_target);
  losec_glGenTextures(1, &g_losec_target);
  losec_glBindTexture(0x0DE1, g_losec_target);
  losec_glTexStorage2D(0x0DE1, 1, 0x8058, w, h);
  losec_glBindFramebuffer(0x8D40, g_losec_fbo);
  losec_glFramebufferTexture2D(0x8D40, 0x8CE0, 0x0DE1, g_losec_target, 0);
  losec_glBindVertexArray(g_losec_vao);
  losec_glBindTexture(0x0DE1, texture);
  losec_glUseProgram(g_losec_program);
  losec_glUniform1i(losec_glGetUniformLocation(g_losec_program, "t"), 0);
  losec_glViewport(0, 0, w, h);
  losec_glDisable(0x0C11);
  losec_glDrawArrays(0x0004, 0, 3);
  unsigned char* pixels = (unsigned char*)malloc((size_t)w * h * 4);
  losec_glReadPixels(0, 0, w, h, 0x1908, 0x1401, pixels);
  free(pixels);
}

static unsigned int losec_texture(void) {
  unsigned int texture = 0;
  losec_glGenTextures(1, &texture);
  losec_glBindTexture(0x0DE1, texture);
  losec_glTexParameteri(0x0DE1, 0x2801, 0x2700);
  losec_glTexParameteri(0x0DE1, 0x2800, 0x2600);
  return texture;
}

static void losec_fill(int w, int h, int levels) {
  for (int round = 0; round < 3; round++) {
    const unsigned int texture = losec_texture();
    losec_glTexStorage2D(0x0DE1, levels, 0x8058, w, h);
    for (int level = 0; level < levels; level++) {
      const int lw = w >> level ? w >> level : 1, lh = h >> level ? h >> level : 1;
      unsigned char* pixels = (unsigned char*)malloc((size_t)lw * lh * 4);
      memset(pixels, 0xAB, (size_t)lw * lh * 4);
      losec_glTexSubImage2D(0x0DE1, level, 0, 0, lw, lh, 0x1908, 0x1401, pixels);
      free(pixels);
    }
    losec_fetch(texture, 1, 1);
    losec_glDeleteTextures(1, &texture);
  }
  losec_glFinish();
}

static void losec_remove_warp_device(void) {
  enum { W = 65, H = 63, LV = 7 };
  static const char* const vertex =
      "#version 300 es\nvoid main(){vec2 p=vec2(gl_VertexID&1,gl_VertexID>>1)*4.0-1.0;gl_Position=vec4(p,0,1);}";
  static const char* const fragment =
      "#version 300 es\nprecision highp float;uniform highp sampler2D t;out highp vec4 o;"
      "void main(){o=texelFetch(t,ivec2(gl_FragCoord.xy),0);}";
  g_losec_program = losec_glCreateProgram();
  const char* const sources[2] = {vertex, fragment};
  const unsigned int kinds[2] = {0x8B31, 0x8B30};
  for (int i = 0; i < 2; i++) {
    const unsigned int shader = losec_glCreateShader(kinds[i]);
    losec_glShaderSource(shader, 1, &sources[i], NULL);
    losec_glCompileShader(shader);
    losec_glAttachShader(g_losec_program, shader);
  }
  losec_glLinkProgram(g_losec_program);
  losec_glGenFramebuffers(1, &g_losec_fbo);
  losec_glGenVertexArrays(1, &g_losec_vao);
  losec_glPixelStorei(0x0D05, 1);
  losec_glPixelStorei(0x0CF5, 1);
  const unsigned int texture = losec_texture();
  losec_glTexStorage2D(0x0DE1, LV, 0x9278, W, H);
  for (int level = 0; level < LV; level++) {
    const int w = W >> level ? W >> level : 1, h = H >> level ? H >> level : 1;
    const int size = ((w + 3) / 4) * ((h + 3) / 4) * 16;
    unsigned char* blocks = (unsigned char*)malloc((size_t)size);
    for (int i = 0; i < size; i++) blocks[i] = (unsigned char)(i * 37 + level);
    losec_glCompressedTexSubImage2D(0x0DE1, level, 0, 0, w, h, 0x9278, size, blocks);
    free(blocks);
  }
  losec_fetch(texture, W, H);
  for (int level = 1; level < 4; level++) {
    losec_glBindTexture(0x0DE1, texture);
    losec_glTexParameteri(0x0DE1, 0x813C, level);
    losec_glFinish();
    losec_fill(W, H, LV);
    losec_fetch(texture, 1, 1);
  }
  losec_glBindTexture(0x0DE1, texture);
  losec_glTexParameteri(0x0DE1, 0x813C, 0);
  losec_glFinish();
  losec_fill(W, H, LV);
  losec_fetch(texture, W, H);
}

static void losec_lose(void* (*lookup)(const char*)) {
  const unsigned long long swaps = g_losec_swaps;
  const double now = losec_now();
  if (g_losec_lost || g_losec_done >= g_losec_times) return;
  if (g_losec_next == 0) g_losec_next = now + g_losec_after;
  if (now < g_losec_next) return;
#define LOSEC_LOAD(ret, name, args) losec_##name = (ret (BRIDGE_MS*) args)lookup(#name);
  LOSEC_GL(LOSEC_LOAD)
  const char* renderer = (const char*)losec_glGetString(0x1F01);
  g_losec_done++;
  if (renderer == NULL || strstr(renderer, "Basic Render") == NULL) {
    fprintf(stderr, "[lose context] loss %d skipped: %s is not the software Direct3D device\n", g_losec_done,
            renderer ? renderer : "(no renderer)");
    return;
  }
  while (losec_glGetError() != 0) {
  }
  losec_remove_warp_device();
  unsigned int error = 0;
  for (int i = 0; i < 16; i++) {
    const unsigned int next = losec_glGetError();
    if (next == 0) break;
    error = next;
    if (next == 0x0507) break;
  }
  g_losec_lost = error == 0x0507;
  g_losec_at = losec_now();
  g_losec_failed_swaps = 0;
  g_losec_frames = 0;
  fprintf(stderr, "[lose context] loss %d: swap %llu, %s (glGetError 0x%04x), the sequence took %.0f ms\n", g_losec_done,
          swaps, g_losec_lost ? "the Direct3D device is removed" : "the device was NOT removed", error,
          (g_losec_at - now) * 1000);
}

static void losec_before_swap(void* (*lookup)(const char*)) {
  losec_config();
  if (g_losec_times == 0) return;
  g_losec_swaps++;
  if (!g_losec_mid_frame) losec_lose(lookup);
}

static void losec_after_swap(bool swapped, void* (*lookup)(const char*)) {
  if (g_losec_times != 0 && g_losec_mid_frame && swapped) {
    const int before = g_losec_lost;
    losec_lose(lookup);
    if (g_losec_lost != before) return;
  }
  if (!g_losec_done) return;
  if (g_losec_lost) {
    if (!swapped) {
      const int failed = ++g_losec_failed_swaps;
      if (failed == 1 || failed == 10 || failed == 100 || failed == 1000 || failed == 10000)
        fprintf(stderr, "[lose context] loss %d: %d swaps failed on the lost context, %.0f ms after the loss\n", g_losec_done,
                failed, (losec_now() - g_losec_at) * 1000);
      return;
    }
    g_losec_lost = 0;
    g_losec_next = losec_now() + g_losec_gap;
    fprintf(stderr, "[lose context] loss %d: a swap works again %.0f ms after the loss, %d failed in between\n", g_losec_done,
            (losec_now() - g_losec_at) * 1000, g_losec_failed_swaps);
  }
  if (swapped && g_losec_at != 0) {
    const unsigned long long frames = ++g_losec_frames;
    if (frames == 1 || frames == 10 || frames == 100 || frames == 1000)
      fprintf(stderr, "[lose context] loss %d: frame %llu after it, %.0f ms after the loss\n", g_losec_done, frames,
              (losec_now() - g_losec_at) * 1000);
  }
}

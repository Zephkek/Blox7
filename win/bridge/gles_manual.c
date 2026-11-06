#define GL_GLEXT_PROTOTYPES 1
#include <GLES3/gl32.h>
#include <GLES2/gl2ext.h>
#include <time.h>

#include "bridge_common.h"

// mesa refuses roblox's uint * int array indexes, android drivers don't care. so we cast

static size_t scaled_index(const char* s, const char* end) {
  if (s >= end || *s != '(') return 0;
  int depth = 0;
  const char* p = s;
  for (; p < end; p++) {
    if (*p == '(') depth++;
    else if (*p == ')' && --depth == 0) break;
    else if (*p == '[' || *p == ']' || *p == ';' || *p == '\n') return 0;
  }
  if (p >= end) return 0;
  const size_t length = (size_t)(p - s) + 1;
  const char* q = p + 1;
  int numbers = 0;
  for (const char* op = "*+"; *op; op++) {
    while (q < end && *q == ' ') q++;
    if (q >= end || *q != *op) return 0;
    q++;
    while (q < end && *q == ' ') q++;
    if (q >= end || *q < '0' || *q > '9') return 0;
    while (q < end && *q >= '0' && *q <= '9') q++;
    numbers++;
  }
  while (q < end && *q == ' ') q++;
  return numbers == 2 && q < end && *q == ']' ? length : 0;
}

void gles_shader_source(void (BRIDGE_MS* real)(GLuint, GLsizei, const GLchar* const*, const GLint*), GLuint shader,
                        GLsizei count, const GLchar* const* strings, const GLint* lengths) {
  if (count < 1 || strings == NULL) {
    real(shader, count, strings, lengths);
    return;
  }
  size_t total = 0, matches = 0;
  for (GLsizei i = 0; i < count; i++) {
    const size_t n = strings[i] ? (lengths && lengths[i] >= 0 ? (size_t)lengths[i] : strlen(strings[i])) : 0;
    const char* s = strings[i];
    for (size_t k = 0; k + 1 < n; k++) {
      if (s[k] == '[' && s[k + 1] == '(' && scaled_index(s + k + 1, s + n)) matches++;
    }
    total += n;
  }
  char* source = matches ? malloc(total + 3 * matches + 1) : NULL;
  if (source == NULL) {
    real(shader, count, strings, lengths);
    return;
  }
  char* out = source;
  for (GLsizei i = 0; i < count; i++) {
    const size_t n = strings[i] ? (lengths && lengths[i] >= 0 ? (size_t)lengths[i] : strlen(strings[i])) : 0;
    const char* s = strings[i];
    for (size_t k = 0; k < n; k++) {
      *out++ = s[k];
      if (s[k] == '[' && k + 1 < n && s[k + 1] == '(' && scaled_index(s + k + 1, s + n)) {
        memcpy(out, "int", 3);
        out += 3;
      }
    }
  }
  *out = '\0';
  const GLchar* rewritten = source;
  real(shader, 1, &rewritten, NULL);
  free(source);
}

enum {
  kCallCompileShader = 0x7f001, kCallLinkProgram, kCallBufferData, kCallBufferSubData, kCallMapBufferRange,
  kCallUnmapBuffer, kCallClientWaitSync, kCallFinish, kCallFlush, kCallReadPixels,
};

static const char* call_name(GLenum code) {
  static const char* const names[] = {"glCompileShader", "glLinkProgram", "glBufferData", "glBufferSubData",
                                      "glMapBufferRange", "glUnmapBuffer", "glClientWaitSync", "glFinish", "glFlush",
                                      "glReadPixels"};
  return code >= kCallCompileShader && code <= kCallReadPixels ? names[code - kCallCompileShader] : NULL;
}

static int tex_stats_enabled(void);
static unsigned long long tex_now_ns(void);
static void tex_note(GLenum format, unsigned long long bytes, unsigned long long start_ns);

void gles_compile_shader(void (BRIDGE_MS* real)(GLuint), GLuint shader) {
  const unsigned long long start = tex_stats_enabled() ? tex_now_ns() : 0;
  real(shader);
  if (start) tex_note(kCallCompileShader, 0, start);
  static int enabled = -1;
  if (enabled < 0) enabled = getenv("BLOX7_GL_SHADER_ERRORS") != NULL;
  if (!enabled) return;
  GLint status = GL_TRUE, length = 0;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
  if (status == GL_TRUE) return;
  glGetShaderiv(shader, GL_SHADER_SOURCE_LENGTH, &length);
  char* source = length > 0 ? malloc((size_t)length) : NULL;
  if (!source) return;
  glGetShaderSource(shader, length, NULL, source);
  fprintf(stderr, "[gles] shader %u does not compile; its source:\n", shader);
  int line = 1;
  for (char* p = source; *p;) {
    char* end = strchr(p, '\n');
    if (end) *end = '\0';
    fprintf(stderr, "[gles] %4d  %s\n", line++, p);
    if (!end) break;
    p = end + 1;
  }
  free(source);
}


struct tex_stat {
  GLenum format;
  unsigned count;
  unsigned long long bytes, ns, max_ns;
};
static struct tex_stat g_tex_stats[24];
static unsigned long long g_tex_window_ns;
static volatile int g_tex_lock;
static int g_tex_nested;

static int tex_stats_enabled(void) {
  static int enabled = -1;
  if (enabled < 0) enabled = getenv("BLOX7_GL_STATS") != NULL || getenv("BLOX7_GL_TEXTURE_STATS") != NULL;
  return enabled;
}



static unsigned long long tex_now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (unsigned long long)ts.tv_sec * 1000000000ull + (unsigned long long)ts.tv_nsec;
}

static void tex_note(GLenum format, unsigned long long bytes, unsigned long long start_ns) {
  const unsigned long long end_ns = tex_now_ns(), ns = end_ns - start_ns;
  while (__atomic_exchange_n(&g_tex_lock, 1, __ATOMIC_ACQUIRE)) {
  }
  if (g_tex_window_ns == 0) g_tex_window_ns = end_ns;
  int i = 0;
  while (i < 24 && g_tex_stats[i].count && g_tex_stats[i].format != format) i++;
  if (i < 24) {
    struct tex_stat* s = &g_tex_stats[i];
    s->format = format;
    s->count++;
    s->bytes += bytes;
    s->ns += ns;
    if (ns > s->max_ns) s->max_ns = ns;
  }
  if (end_ns - g_tex_window_ns >= 5000000000ull) {
    for (int k = 0; k < 24 && g_tex_stats[k].count; k++) {
      const struct tex_stat* s = &g_tex_stats[k];
      if (call_name(s->format)) {
        fprintf(stderr, "[gl-call] %s: %u calls, %.1f MB, %.1f ms, longest %.2f ms\n", call_name(s->format), s->count,
                (double)s->bytes / 1048576.0, (double)s->ns / 1e6, (double)s->max_ns / 1e6);
      } else {
        fprintf(stderr, "[gl-tex] format 0x%04x: %u uploads, %.1f MB, %.1f ms in the driver, longest %.2f ms\n",
                s->format, s->count, (double)s->bytes / 1048576.0, (double)s->ns / 1e6, (double)s->max_ns / 1e6);
      }
    }
    memset(g_tex_stats, 0, sizeof g_tex_stats);
    g_tex_window_ns = end_ns;
  }
  __atomic_store_n(&g_tex_lock, 0, __ATOMIC_RELEASE);
}

static unsigned long long tex_pixel_bytes(GLenum format, GLenum type) {
  unsigned components = format == GL_RGBA || format == GL_RGBA_INTEGER ? 4 : format == GL_RGB || format == GL_RGB_INTEGER ? 3
                      : format == GL_RG || format == GL_RG_INTEGER ? 2 : 1;
  switch (type) {
    case GL_UNSIGNED_SHORT_5_6_5: case GL_UNSIGNED_SHORT_4_4_4_4: case GL_UNSIGNED_SHORT_5_5_5_1: return 2;
    case GL_UNSIGNED_INT_2_10_10_10_REV: case GL_UNSIGNED_INT_10F_11F_11F_REV: case GL_UNSIGNED_INT_5_9_9_9_REV:
    case GL_UNSIGNED_INT_24_8: return 4;
    case GL_HALF_FLOAT: case GL_SHORT: case GL_UNSIGNED_SHORT: return components * 2ull;
    case GL_FLOAT: case GL_INT: case GL_UNSIGNED_INT: return components * 4ull;
    default: return components;
  }
}

void gles_tex_image_2d(void (BRIDGE_MS* real)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*),
                       GLenum target, GLint level, GLint internal, GLsizei w, GLsizei h, GLint border, GLenum format,
                       GLenum type, const void* pixels) {
  if (!tex_stats_enabled() || !pixels || g_tex_nested) {
    real(target, level, internal, w, h, border, format, type, pixels);
    return;
  }
  const unsigned long long start = tex_now_ns();
  real(target, level, internal, w, h, border, format, type, pixels);
  tex_note((GLenum)internal, (unsigned long long)w * (unsigned long long)h * tex_pixel_bytes(format, type), start);
}

void gles_tex_sub_image_2d(void (BRIDGE_MS* real)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void*),
                           GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type,
                           const void* pixels) {
  if (!tex_stats_enabled() || g_tex_nested) {
    real(target, level, x, y, w, h, format, type, pixels);
    return;
  }
  const unsigned long long start = tex_now_ns();
  real(target, level, x, y, w, h, format, type, pixels);
  tex_note(format | 0x10000u, (unsigned long long)w * (unsigned long long)h * tex_pixel_bytes(format, type), start);
}

// vmware has no etc2 so mesa decodes it texel by texel on the render thread. 4 threads go brrr

GLenum gles_etc_storage_format(GLenum format, int* decoder);
void* gles_etc_decode(int decoder, const void* data, size_t size, int w, int h, int layers);


struct unpack_state {
  GLint alignment, row_length, image_height, skip_rows, skip_pixels, skip_images, buffer;
};

static void unpack_tight(struct unpack_state* s) {
  glGetIntegerv(GL_UNPACK_ALIGNMENT, &s->alignment);
  glGetIntegerv(GL_UNPACK_ROW_LENGTH, &s->row_length);
  glGetIntegerv(GL_UNPACK_IMAGE_HEIGHT, &s->image_height);
  glGetIntegerv(GL_UNPACK_SKIP_ROWS, &s->skip_rows);
  glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &s->skip_pixels);
  glGetIntegerv(GL_UNPACK_SKIP_IMAGES, &s->skip_images);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
  glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
  glPixelStorei(GL_UNPACK_IMAGE_HEIGHT, 0);
  glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
  glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
  glPixelStorei(GL_UNPACK_SKIP_IMAGES, 0);
  if (s->buffer) glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
}

static void unpack_restore(const struct unpack_state* s) {
  glPixelStorei(GL_UNPACK_ALIGNMENT, s->alignment);
  glPixelStorei(GL_UNPACK_ROW_LENGTH, s->row_length);
  glPixelStorei(GL_UNPACK_IMAGE_HEIGHT, s->image_height);
  glPixelStorei(GL_UNPACK_SKIP_ROWS, s->skip_rows);
  glPixelStorei(GL_UNPACK_SKIP_PIXELS, s->skip_pixels);
  glPixelStorei(GL_UNPACK_SKIP_IMAGES, s->skip_images);
  if (s->buffer) glBindBuffer(GL_PIXEL_UNPACK_BUFFER, (GLuint)s->buffer);
}

static void* etc_texels(int decoder, const void* data, GLsizei size, GLsizei w, GLsizei h, GLsizei layers,
                        struct unpack_state* s) {
  s->buffer = 0;
  glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &s->buffer);
  if (!s->buffer) {
    if (!data) return NULL;
    const unsigned long long start = tex_stats_enabled() ? tex_now_ns() : 0;
    void* texels = gles_etc_decode(decoder, data, (size_t)size, w, h, layers);
    if (start) tex_note((GLenum)decoder | 0x60000u, (unsigned long long)size, start);
    return texels;
  }
  const void* mapped = glMapBufferRange(GL_PIXEL_UNPACK_BUFFER, (GLintptr)(uintptr_t)data, size, GL_MAP_READ_BIT);
  if (!mapped) return NULL;
  void* texels = gles_etc_decode(decoder, mapped, (size_t)size, w, h, layers);
  glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
  return texels;
}

void gles_compressed_tex_image_2d(void (BRIDGE_MS* real)(GLenum, GLint, GLenum, GLsizei, GLsizei, GLint, GLsizei, const void*),
                                  GLenum target, GLint level, GLenum internal, GLsizei w, GLsizei h, GLint border,
                                  GLsizei size, const void* data) {
  const unsigned long long start = tex_stats_enabled() ? tex_now_ns() : 0;
  int decoder;
  const GLenum storage = gles_etc_storage_format(internal, &decoder);
  if (storage) {
    struct unpack_state s;
    void* texels = etc_texels(decoder, data, size, w, h, 1, &s);
    if (texels || (!data && !s.buffer)) {
      unpack_tight(&s);
      g_tex_nested = 1;
      glTexImage2D(target, level, (GLint)storage, w, h, border, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
      if (texels) glTexSubImage2D(target, level, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, texels);
      g_tex_nested = 0;
      unpack_restore(&s);
      if (start) tex_note(internal | 0x40000u, (unsigned long long)size, start);
      return;
    }
  }
  real(target, level, internal, w, h, border, size, data);
  if (start) tex_note(internal, (unsigned long long)size, start);
}

void gles_compressed_tex_sub_image_2d(void (BRIDGE_MS* real)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLsizei,
                                                              const void*),
                                      GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h, GLenum format,
                                      GLsizei size, const void* data) {
  const unsigned long long start = tex_stats_enabled() ? tex_now_ns() : 0;
  int decoder;
  if (gles_etc_storage_format(format, &decoder)) {
    struct unpack_state s;
    void* texels = etc_texels(decoder, data, size, w, h, 1, &s);
    if (texels) {
      unpack_tight(&s);
      g_tex_nested = 1;
      glTexSubImage2D(target, level, x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, texels);
      g_tex_nested = 0;
      unpack_restore(&s);
      if (start) tex_note(format | 0x40000u, (unsigned long long)size, start);
      return;
    }
  }
  real(target, level, x, y, w, h, format, size, data);
  if (start) tex_note(format, (unsigned long long)size, start);
}

void gles_compressed_tex_image_3d(void (BRIDGE_MS* real)(GLenum, GLint, GLenum, GLsizei, GLsizei, GLsizei, GLint, GLsizei,
                                                          const void*),
                                  GLenum target, GLint level, GLenum internal, GLsizei w, GLsizei h, GLsizei d,
                                  GLint border, GLsizei size, const void* data) {
  const unsigned long long start = tex_stats_enabled() ? tex_now_ns() : 0;
  int decoder;
  const GLenum storage = gles_etc_storage_format(internal, &decoder);
  if (storage && d > 0) {
    struct unpack_state s;
    void* texels = etc_texels(decoder, data, size, w, h, d, &s);
    if (texels || (!data && !s.buffer)) {
      unpack_tight(&s);
      g_tex_nested = 1;
      glTexImage3D(target, level, (GLint)storage, w, h, d, border, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
      if (texels) glTexSubImage3D(target, level, 0, 0, 0, w, h, d, GL_RGBA, GL_UNSIGNED_BYTE, texels);
      g_tex_nested = 0;
      unpack_restore(&s);
      if (start) tex_note(internal | 0x50000u, (unsigned long long)size, start);
      return;
    }
  }
  real(target, level, internal, w, h, d, border, size, data);
  if (start) tex_note(internal | 0x20000u, (unsigned long long)size, start);
}

void gles_compressed_tex_sub_image_3d(void (BRIDGE_MS* real)(GLenum, GLint, GLint, GLint, GLint, GLsizei, GLsizei, GLsizei,
                                                              GLenum, GLsizei, const void*),
                                      GLenum target, GLint level, GLint x, GLint y, GLint z, GLsizei w, GLsizei h,
                                      GLsizei d, GLenum format, GLsizei size, const void* data) {
  const unsigned long long start = tex_stats_enabled() ? tex_now_ns() : 0;
  int decoder;
  if (gles_etc_storage_format(format, &decoder) && d > 0) {
    struct unpack_state s;
    void* texels = etc_texels(decoder, data, size, w, h, d, &s);
    if (texels) {
      unpack_tight(&s);
      g_tex_nested = 1;
      glTexSubImage3D(target, level, x, y, z, w, h, d, GL_RGBA, GL_UNSIGNED_BYTE, texels);
      g_tex_nested = 0;
      unpack_restore(&s);
      if (start) tex_note(format | 0x50000u, (unsigned long long)size, start);
      return;
    }
  }
  real(target, level, x, y, z, w, h, d, format, size, data);
  if (start) tex_note(format | 0x20000u, (unsigned long long)size, start);
}

void gles_tex_storage_2d(void (BRIDGE_MS* real)(GLenum, GLsizei, GLenum, GLsizei, GLsizei), GLenum target, GLsizei levels,
                         GLenum internal, GLsizei w, GLsizei h) {
  int decoder;
  const GLenum storage = gles_etc_storage_format(internal, &decoder);
  real(target, levels, storage ? storage : internal, w, h);
}


void gles_link_program(void (BRIDGE_MS* real)(GLuint), GLuint program) {
  const unsigned long long start = tex_stats_enabled() ? tex_now_ns() : 0;
  real(program);
  if (start) tex_note(kCallLinkProgram, 0, start);
}

void gles_buffer_data(void (BRIDGE_MS* real)(GLenum, GLsizeiptr, const void*, GLenum), GLenum target, GLsizeiptr size,
                      const void* data, GLenum usage) {
  const unsigned long long start = tex_stats_enabled() ? tex_now_ns() : 0;
  real(target, size, data, usage);
  if (start) tex_note(kCallBufferData, (unsigned long long)size, start);
}

void gles_buffer_sub_data(void (BRIDGE_MS* real)(GLenum, GLintptr, GLsizeiptr, const void*), GLenum target,
                          GLintptr offset, GLsizeiptr size, const void* data) {
  const unsigned long long start = tex_stats_enabled() ? tex_now_ns() : 0;
  real(target, offset, size, data);
  if (start) tex_note(kCallBufferSubData, (unsigned long long)size, start);
}

// tried orphaning roblox's buffer maps: 5x faster in texbench, way worse in bedwars. don't
void* gles_map_buffer_range(void* (BRIDGE_MS* real)(GLenum, GLintptr, GLsizeiptr, GLbitfield), GLenum target,
                            GLintptr offset, GLsizeiptr length, GLbitfield access) {
  const unsigned long long start = tex_stats_enabled() ? tex_now_ns() : 0;
  void* mapped = real(target, offset, length, access);
  if (start) {
    tex_note(kCallMapBufferRange, (unsigned long long)length, start);
    const unsigned long long ns = tex_now_ns() - start;
    if (ns > 4000000) {
      GLint buffer = 0, size = 0, usage = 0;
      const GLenum binding = target == GL_ARRAY_BUFFER ? GL_ARRAY_BUFFER_BINDING
                           : target == GL_ELEMENT_ARRAY_BUFFER ? GL_ELEMENT_ARRAY_BUFFER_BINDING
                           : target == GL_UNIFORM_BUFFER ? GL_UNIFORM_BUFFER_BINDING
                           : target == GL_COPY_WRITE_BUFFER ? GL_COPY_WRITE_BUFFER_BINDING
                           : target == GL_PIXEL_UNPACK_BUFFER ? GL_PIXEL_UNPACK_BUFFER_BINDING : 0;
      if (binding) glGetIntegerv(binding, &buffer);
      glGetBufferParameteriv(target, GL_BUFFER_SIZE, &size);
      glGetBufferParameteriv(target, GL_BUFFER_USAGE, &usage);
      fprintf(stderr, "[gl-map] %.1f ms: target 0x%x buffer %d (%d bytes, usage 0x%x) offset %lld length %lld access 0x%x\n",
              (double)ns / 1e6, target, buffer, size, usage, (long long)offset, (long long)length, access);
    }
  }
  return mapped;
}

GLboolean gles_unmap_buffer(GLboolean (BRIDGE_MS* real)(GLenum), GLenum target) {
  const unsigned long long start = tex_stats_enabled() ? tex_now_ns() : 0;
  const GLboolean ok = real(target);
  if (start) tex_note(kCallUnmapBuffer, 0, start);
  return ok;
}

GLenum gles_client_wait_sync(GLenum (BRIDGE_MS* real)(GLsync, GLbitfield, GLuint64), GLsync sync, GLbitfield flags,
                             GLuint64 timeout) {
  const unsigned long long start = tex_stats_enabled() ? tex_now_ns() : 0;
  const GLenum result = real(sync, flags, timeout);
  if (start) tex_note(kCallClientWaitSync, 0, start);
  return result;
}

void gles_finish(void (BRIDGE_MS* real)(void)) {
  const unsigned long long start = tex_stats_enabled() ? tex_now_ns() : 0;
  real();
  if (start) tex_note(kCallFinish, 0, start);
}

void gles_flush(void (BRIDGE_MS* real)(void)) {
  const unsigned long long start = tex_stats_enabled() ? tex_now_ns() : 0;
  real();
  if (start) tex_note(kCallFlush, 0, start);
}

void gles_read_pixels(void (BRIDGE_MS* real)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*), GLint x, GLint y,
                      GLsizei w, GLsizei h, GLenum format, GLenum type, void* pixels) {
  const unsigned long long start = tex_stats_enabled() ? tex_now_ns() : 0;
  real(x, y, w, h, format, type, pixels);
  if (start) tex_note(kCallReadPixels, (unsigned long long)w * (unsigned long long)h * 4, start);
}

void gles_tex_storage_3d(void (BRIDGE_MS* real)(GLenum, GLsizei, GLenum, GLsizei, GLsizei, GLsizei), GLenum target,
                         GLsizei levels, GLenum internal, GLsizei w, GLsizei h, GLsizei d) {
  int decoder;
  const GLenum storage = gles_etc_storage_format(internal, &decoder);
  real(target, levels, storage ? storage : internal, w, h, d);
}

void gles_tex_sub_image_3d(void (BRIDGE_MS* real)(GLenum, GLint, GLint, GLint, GLint, GLsizei, GLsizei, GLsizei, GLenum, GLenum,
                                                   const void*),
                           GLenum target, GLint level, GLint x, GLint y, GLint z, GLsizei w, GLsizei h, GLsizei d,
                           GLenum format, GLenum type, const void* pixels) {
  if (!tex_stats_enabled() || g_tex_nested) {
    real(target, level, x, y, z, w, h, d, format, type, pixels);
    return;
  }
  const unsigned long long start = tex_now_ns();
  real(target, level, x, y, z, w, h, d, format, type, pixels);
  tex_note(format | 0x30000u, (unsigned long long)w * (unsigned long long)h * (unsigned long long)d * tex_pixel_bytes(format, type),
           start);
}

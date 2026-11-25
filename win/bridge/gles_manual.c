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

static size_t g_etc_live;
static GLuint bound_texture(GLenum target);
static void etc_set(GLuint name, GLenum format);

void gles_tex_image_2d(void (BRIDGE_MS* real)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*),
                       GLenum target, GLint level, GLint internal, GLsizei w, GLsizei h, GLint border, GLenum format,
                       GLenum type, const void* pixels) {
  if (g_etc_live && !g_tex_nested) etc_set(bound_texture(target), 0);
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

struct etc_entry {
  GLuint name;
  GLenum format;
};

static struct etc_entry* g_etc_tex;
static size_t g_etc_cap, g_etc_used, g_etc_live;

static GLenum texture_binding(GLenum target) {
  switch (target) {
    case GL_TEXTURE_2D: return GL_TEXTURE_BINDING_2D;
    case GL_TEXTURE_3D: return GL_TEXTURE_BINDING_3D;
    case GL_TEXTURE_2D_ARRAY: return GL_TEXTURE_BINDING_2D_ARRAY;
    case GL_TEXTURE_CUBE_MAP_ARRAY: return GL_TEXTURE_BINDING_CUBE_MAP_ARRAY;
    case GL_TEXTURE_CUBE_MAP:
    case GL_TEXTURE_CUBE_MAP_POSITIVE_X:
    case GL_TEXTURE_CUBE_MAP_NEGATIVE_X:
    case GL_TEXTURE_CUBE_MAP_POSITIVE_Y:
    case GL_TEXTURE_CUBE_MAP_NEGATIVE_Y:
    case GL_TEXTURE_CUBE_MAP_POSITIVE_Z:
    case GL_TEXTURE_CUBE_MAP_NEGATIVE_Z: return GL_TEXTURE_BINDING_CUBE_MAP;
    default: return 0;
  }
}

static GLuint bound_texture(GLenum target) {
  const GLenum binding = texture_binding(target);
  GLint name = 0;
  if (binding) glGetIntegerv(binding, &name);
  return (GLuint)name;
}

static size_t etc_slot(GLuint name) {
  size_t i = ((size_t)name * 2654435761u) & (g_etc_cap - 1);
  while (g_etc_tex[i].name && g_etc_tex[i].name != name) i = (i + 1) & (g_etc_cap - 1);
  return i;
}

static GLenum etc_format_of(GLuint name) {
  if (!name || !g_etc_live) return 0;
  return g_etc_tex[etc_slot(name)].format;
}

static void etc_set(GLuint name, GLenum format) {
  if (!name || (!format && !g_etc_live)) return;
  if ((g_etc_used + 1) * 2 > g_etc_cap) {
    const size_t old_cap = g_etc_cap;
    struct etc_entry* old = g_etc_tex;
    const size_t cap = old_cap ? old_cap * 2 : 256;
    struct etc_entry* table = calloc(cap, sizeof *table);
    if (!table) return;
    g_etc_tex = table;
    g_etc_cap = cap;
    g_etc_used = g_etc_live = 0;
    for (size_t i = 0; i < old_cap; i++)
      if (old[i].name && old[i].format) {
        g_etc_tex[etc_slot(old[i].name)] = old[i];
        g_etc_used++;
        g_etc_live++;
      }
    free(old);
  }
  struct etc_entry* e = &g_etc_tex[etc_slot(name)];
  if (!e->name) {
    if (!format) return;
    e->name = name;
    g_etc_used++;
  }
  if (e->format && !format) g_etc_live--;
  if (!e->format && format) g_etc_live++;
  e->format = format;
}

static void raise_invalid_value(void) { glLineWidth(0.0f); }

static void raise_invalid_operation(void) {
  GLint active = 0;
  glGetIntegerv(GL_TRANSFORM_FEEDBACK_ACTIVE, &active);
  if (active) glBeginTransformFeedback(GL_POINTS);
  else glEndTransformFeedback();
}

static int etc_block_bytes(GLenum format) {
  return format == GL_COMPRESSED_RGBA8_ETC2_EAC || format == GL_COMPRESSED_SRGB8_ALPHA8_ETC2_EAC ? 16 : 8;
}

static int etc_size_valid(GLenum format, GLsizei w, GLsizei h, GLsizei d, GLsizei size) {
  if (w < 0 || h < 0 || d < 0 || size < 0) return 0;
  return (long long)((w + 3) / 4) * ((h + 3) / 4) * d * etc_block_bytes(format) == (long long)size;
}

static int etc_source_valid(const void* data, GLsizei size) {
  GLint buffer = 0;
  glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &buffer);
  if (!buffer) return 1;
  GLint mapped = 0;
  GLint64 length = 0;
  glGetBufferParameteriv(GL_PIXEL_UNPACK_BUFFER, GL_BUFFER_MAPPED, &mapped);
  glGetBufferParameteri64v(GL_PIXEL_UNPACK_BUFFER, GL_BUFFER_SIZE, &length);
  return !mapped && (long long)(uintptr_t)data + size <= length;
}

static int etc_sub_valid(GLenum target, GLenum format, GLsizei w, GLsizei h, GLsizei d, GLsizei size, const void* data,
                         int* use_driver) {
  *use_driver = 0;
  if (etc_format_of(bound_texture(target)) != format || !etc_source_valid(data, size)) {
    *use_driver = 1;
    return 0;
  }
  if (!etc_size_valid(format, w, h, d, size)) {
    raise_invalid_value();
    return 0;
  }
  return 1;
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
  if (storage && etc_size_valid(internal, w, h, 1, size) && etc_source_valid(data, size)) {
    struct unpack_state s;
    void* texels = etc_texels(decoder, data, size, w, h, 1, &s);
    if (texels || (!data && !s.buffer)) {
      unpack_tight(&s);
      g_tex_nested = 1;
      glTexImage2D(target, level, (GLint)storage, w, h, border, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
      if (texels) glTexSubImage2D(target, level, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, texels);
      g_tex_nested = 0;
      unpack_restore(&s);
      etc_set(bound_texture(target), internal);
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
  int use_driver = 1;
  if (gles_etc_storage_format(format, &decoder) && !etc_sub_valid(target, format, w, h, 1, size, data, &use_driver) &&
      !use_driver)
    return;
  if (!use_driver) {
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
  if (storage && d > 0 && etc_size_valid(internal, w, h, d, size) && etc_source_valid(data, size)) {
    struct unpack_state s;
    void* texels = etc_texels(decoder, data, size, w, h, d, &s);
    if (texels || (!data && !s.buffer)) {
      unpack_tight(&s);
      g_tex_nested = 1;
      glTexImage3D(target, level, (GLint)storage, w, h, d, border, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
      if (texels) glTexSubImage3D(target, level, 0, 0, 0, w, h, d, GL_RGBA, GL_UNSIGNED_BYTE, texels);
      g_tex_nested = 0;
      unpack_restore(&s);
      etc_set(bound_texture(target), internal);
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
  int use_driver = 1;
  if (gles_etc_storage_format(format, &decoder) && d > 0 &&
      !etc_sub_valid(target, format, w, h, d, size, data, &use_driver) && !use_driver)
    return;
  if (!use_driver) {
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
  if (storage || g_etc_live) etc_set(bound_texture(target), storage ? internal : 0);
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
  if (storage || g_etc_live) etc_set(bound_texture(target), storage ? internal : 0);
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

#define GL_BACK_LEFT 0x0402

static int g_window_srgb, g_window_checked;
static int g_copy_route;
static int g_marker_state;

void gles_window_reset(void) {
  g_window_checked = 0;
  g_window_srgb = 0;
  g_copy_route = 0;
  g_marker_state = 0;
}

static int has_extension(const char* list, const char* name) {
  const size_t n = strlen(name);
  for (const char* p = list; (p = strstr(p, name)) != NULL; p += n)
    if ((p == list || p[-1] == ' ') && (p[n] == ' ' || p[n] == '\0')) return 1;
  return 0;
}

static const char kMarkerExtension[] = "GL_EXT_debug_marker";
enum { kMarkerNative = 1, kMarkerAdded = 2 };

static const GLubyte* (BRIDGE_MS* g_real_get_string)(GLenum);
static void (BRIDGE_MS* g_real_insert_marker)(GLsizei, const GLchar*);
static void (BRIDGE_MS* g_real_push_marker)(GLsizei, const GLchar*);
static void (BRIDGE_MS* g_real_pop_marker)(void);

static int marker_state(void) {
  if (!g_marker_state && g_real_get_string) {
    const char* list = (const char*)g_real_get_string(GL_EXTENSIONS);
    if (list) g_marker_state = has_extension(list, kMarkerExtension) ? kMarkerNative : kMarkerAdded;
  }
  return g_marker_state;
}

static int marker_added(void) { return marker_state() == kMarkerAdded; }

const GLubyte* gles_get_string(const GLubyte* (BRIDGE_MS* real)(GLenum), GLenum name) {
  const GLubyte* value = real(name);
  if (name != GL_EXTENSIONS || !value || !marker_added()) return value;
  static char* listed[4];
  static volatile int lock;
  const size_t n = strlen((const char*)value);
  char* found = NULL;
  while (__atomic_exchange_n(&lock, 1, __ATOMIC_ACQUIRE)) {
  }
  for (int i = 0; i < 4 && !found; i++) {
    if (listed[i] && strcmp(listed[i] + sizeof kMarkerExtension, (const char*)value) == 0) found = listed[i];
    else if (!listed[i] && (listed[i] = malloc(sizeof kMarkerExtension + n + 1)) != NULL) {
      found = listed[i];
      memcpy(found, kMarkerExtension, sizeof kMarkerExtension - 1);
      found[sizeof kMarkerExtension - 1] = ' ';
      memcpy(found + sizeof kMarkerExtension, value, n + 1);
    }
  }
  __atomic_store_n(&lock, 0, __ATOMIC_RELEASE);
  return found ? (const GLubyte*)found : value;
}

const GLubyte* gles_get_stringi(const GLubyte* (BRIDGE_MS* real)(GLenum, GLuint), GLenum name, GLuint index) {
  if (name != GL_EXTENSIONS || !marker_added()) return real(name, index);
  return index ? real(name, index - 1) : (const GLubyte*)kMarkerExtension;
}

static int extension_count(void (BRIDGE_MS* real)(GLenum, GLint*)) {
  GLint count = -1;
  real(GL_NUM_EXTENSIONS, &count);
  return count < 0 ? -1 : count + 1;
}

void gles_get_integerv(void (BRIDGE_MS* real)(GLenum, GLint*), GLenum pname, GLint* data) {
  if (pname != GL_NUM_EXTENSIONS || !data || !marker_added()) {
    real(pname, data);
    return;
  }
  const int count = extension_count(real);
  if (count >= 0) *data = count;
}

static void (BRIDGE_MS* g_real_get_integerv)(GLenum, GLint*);

void gles_get_integer64v(void (BRIDGE_MS* real)(GLenum, GLint64*), GLenum pname, GLint64* data) {
  if (pname != GL_NUM_EXTENSIONS || !data || !g_real_get_integerv || !marker_added()) {
    real(pname, data);
    return;
  }
  const int count = extension_count(g_real_get_integerv);
  if (count >= 0) *data = count;
}

void gles_get_floatv(void (BRIDGE_MS* real)(GLenum, GLfloat*), GLenum pname, GLfloat* data) {
  if (pname != GL_NUM_EXTENSIONS || !data || !g_real_get_integerv || !marker_added()) {
    real(pname, data);
    return;
  }
  const int count = extension_count(g_real_get_integerv);
  if (count >= 0) *data = (GLfloat)count;
}

void gles_get_booleanv(void (BRIDGE_MS* real)(GLenum, GLboolean*), GLenum pname, GLboolean* data) {
  if (pname != GL_NUM_EXTENSIONS || !data || !g_real_get_integerv || !marker_added()) {
    real(pname, data);
    return;
  }
  if (extension_count(g_real_get_integerv) >= 0) *data = GL_TRUE;
}

BRIDGE_EXPORT void GL_APIENTRY glInsertEventMarkerEXT(GLsizei length, const GLchar* marker) {
  if (g_real_insert_marker && marker_state() == kMarkerNative) g_real_insert_marker(length, marker);
}

BRIDGE_EXPORT void GL_APIENTRY glPushGroupMarkerEXT(GLsizei length, const GLchar* marker) {
  if (g_real_push_marker && marker_state() == kMarkerNative) g_real_push_marker(length, marker);
}

BRIDGE_EXPORT void GL_APIENTRY glPopGroupMarkerEXT(void) {
  if (g_real_pop_marker && marker_state() == kMarkerNative) g_real_pop_marker();
}

int gles_bridge_always_has(const char* name) {
  return strcmp(name, "glInsertEventMarkerEXT") == 0 || strcmp(name, "glPushGroupMarkerEXT") == 0 ||
         strcmp(name, "glPopGroupMarkerEXT") == 0;
}

static void cube_face_target(GLenum* target, GLint* z) {
  if (*target < GL_TEXTURE_CUBE_MAP_POSITIVE_X || *target > GL_TEXTURE_CUBE_MAP_NEGATIVE_Z) return;
  *z += (GLint)(*target - GL_TEXTURE_CUBE_MAP_POSITIVE_X);
  *target = GL_TEXTURE_CUBE_MAP;
}

void gles_copy_image_sub_data(void (BRIDGE_MS* real)(GLuint, GLenum, GLint, GLint, GLint, GLint, GLuint, GLenum, GLint,
                                                     GLint, GLint, GLint, GLsizei, GLsizei, GLsizei),
                              GLuint src, GLenum src_target, GLint src_level, GLint src_x, GLint src_y, GLint src_z,
                              GLuint dst, GLenum dst_target, GLint dst_level, GLint dst_x, GLint dst_y, GLint dst_z,
                              GLsizei width, GLsizei height, GLsizei depth) {
  cube_face_target(&src_target, &src_z);
  cube_face_target(&dst_target, &dst_z);
  real(src, src_target, src_level, src_x, src_y, src_z, dst, dst_target, dst_level, dst_x, dst_y, dst_z, width, height,
       depth);
}

void gles_copy_image_sub_data_core(void (BRIDGE_MS* real)(GLuint, GLenum, GLint, GLint, GLint, GLint, GLuint, GLenum,
                                                          GLint, GLint, GLint, GLint, GLsizei, GLsizei, GLsizei),
                                   GLuint src, GLenum src_target, GLint src_level, GLint src_x, GLint src_y,
                                   GLint src_z, GLuint dst, GLenum dst_target, GLint dst_level, GLint dst_x,
                                   GLint dst_y, GLint dst_z, GLsizei width, GLsizei height, GLsizei depth) {
  if (!g_copy_route) {
    GLint major = 0, minor = 0;
    glGetIntegerv(GL_MAJOR_VERSION, &major);
    glGetIntegerv(GL_MINOR_VERSION, &minor);
    const char* list = (const char*)glGetString(GL_EXTENSIONS);
    if (major > 3 || (major == 3 && minor >= 2) || !list) g_copy_route = 1;
    else if (has_extension(list, "GL_EXT_copy_image")) g_copy_route = 2;
    else if (has_extension(list, "GL_OES_copy_image")) g_copy_route = 3;
    else g_copy_route = 1;
  }
  if (g_copy_route == 2)
    glCopyImageSubDataEXT(src, src_target, src_level, src_x, src_y, src_z, dst, dst_target, dst_level, dst_x, dst_y,
                          dst_z, width, height, depth);
  else if (g_copy_route == 3)
    glCopyImageSubDataOES(src, src_target, src_level, src_x, src_y, src_z, dst, dst_target, dst_level, dst_x, dst_y,
                          dst_z, width, height, depth);
  else
    gles_copy_image_sub_data(real, src, src_target, src_level, src_x, src_y, src_z, dst, dst_target, dst_level, dst_x,
                             dst_y, dst_z, width, height, depth);
}

void gles_generate_mipmap(void (BRIDGE_MS* real)(GLenum), GLenum target) {
  if (g_etc_live && etc_format_of(bound_texture(target))) {
    raise_invalid_operation();
    return;
  }
  real(target);
}

void gles_delete_textures(void (BRIDGE_MS* real)(GLsizei, const GLuint*), GLsizei n, const GLuint* textures) {
  if (g_etc_live && textures)
    for (GLsizei i = 0; i < n; i++) etc_set(textures[i], 0);
  real(n, textures);
}

static void (BRIDGE_MS* g_get_level_iv)(GLenum, GLint, GLenum, GLint*);

static GLenum etc_level_format(void (BRIDGE_MS* real)(GLenum, GLint, GLenum, GLint*), GLenum target, GLint level) {
  if (!g_etc_live) return 0;
  const GLenum format = etc_format_of(bound_texture(target));
  if (!format) return 0;
  int decoder;
  GLint internal = 0;
  real(target, level, GL_TEXTURE_INTERNAL_FORMAT, &internal);
  return (GLenum)internal == gles_etc_storage_format(format, &decoder) ? format : 0;
}

void gles_get_tex_level_parameteriv(void (BRIDGE_MS* real)(GLenum, GLint, GLenum, GLint*), GLenum target, GLint level,
                                    GLenum pname, GLint* params) {
  real(target, level, pname, params);
  if (!params || (pname != GL_TEXTURE_COMPRESSED && pname != GL_TEXTURE_INTERNAL_FORMAT)) return;
  const GLenum format = etc_level_format(real, target, level);
  if (format) *params = pname == GL_TEXTURE_COMPRESSED ? GL_TRUE : (GLint)format;
}

void gles_get_tex_level_parameterfv(void (BRIDGE_MS* real)(GLenum, GLint, GLenum, GLfloat*), GLenum target,
                                    GLint level, GLenum pname, GLfloat* params) {
  real(target, level, pname, params);
  if (!params || !g_get_level_iv || (pname != GL_TEXTURE_COMPRESSED && pname != GL_TEXTURE_INTERNAL_FORMAT)) return;
  const GLenum format = etc_level_format(g_get_level_iv, target, level);
  if (format) *params = pname == GL_TEXTURE_COMPRESSED ? 1.0f : (GLfloat)format;
}

static GLDEBUGPROC g_debug_proc;
static const void* g_debug_user;
static void (BRIDGE_MS* g_real_debug_callback)(void*, const void*);
static void (BRIDGE_MS* g_real_debug_callback_khr)(void*, const void*);

static BRIDGE_MS void debug_trampoline(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length,
                                       const GLchar* message, const void* user) {
  (void)user;
  const GLDEBUGPROC proc = g_debug_proc;
  if (!proc) return;
  bridge_attach();
  proc(source, type, id, severity, length, message, g_debug_user);
}

void gles_bridge_resolve_manual(void* (*lookup)(const char*)) {
  g_real_debug_callback = (void (BRIDGE_MS*)(void*, const void*))lookup("glDebugMessageCallback");
  g_real_debug_callback_khr = (void (BRIDGE_MS*)(void*, const void*))lookup("glDebugMessageCallbackKHR");
  g_get_level_iv = (void (BRIDGE_MS*)(GLenum, GLint, GLenum, GLint*))lookup("glGetTexLevelParameteriv");
  g_real_get_string = (const GLubyte* (BRIDGE_MS*)(GLenum))lookup("glGetString");
  g_real_get_integerv = (void (BRIDGE_MS*)(GLenum, GLint*))lookup("glGetIntegerv");
  g_real_insert_marker = (void (BRIDGE_MS*)(GLsizei, const GLchar*))lookup("glInsertEventMarkerEXT");
  g_real_push_marker = (void (BRIDGE_MS*)(GLsizei, const GLchar*))lookup("glPushGroupMarkerEXT");
  g_real_pop_marker = (void (BRIDGE_MS*)(void))lookup("glPopGroupMarkerEXT");
}

static void set_debug_callback(void (BRIDGE_MS* real)(void*, const void*), GLDEBUGPROC callback, const void* user) {
  if (!real) real = g_real_debug_callback ? g_real_debug_callback : g_real_debug_callback_khr;
  if (!real) return;
  g_debug_proc = callback;
  g_debug_user = user;
  real(callback ? (void*)debug_trampoline : NULL, NULL);
}

BRIDGE_EXPORT void GL_APIENTRY glDebugMessageCallback(GLDEBUGPROC callback, const void* user) {
  set_debug_callback(g_real_debug_callback, callback, user);
}

BRIDGE_EXPORT void GL_APIENTRY glDebugMessageCallbackKHR(GLDEBUGPROCKHR callback, const void* user) {
  set_debug_callback(g_real_debug_callback_khr, (GLDEBUGPROC)callback, user);
}

void gles_get_pointerv(void (BRIDGE_MS* real)(GLenum, void**), GLenum pname, void** params) {
  if (params && pname == GL_DEBUG_CALLBACK_FUNCTION) {
    *params = (void*)g_debug_proc;
    return;
  }
  if (params && pname == GL_DEBUG_CALLBACK_USER_PARAM) {
    *params = (void*)g_debug_user;
    return;
  }
  real(pname, params);
}

static int gles_refused(void) {
  int refused = 0;
  for (int i = 0; i < 8 && glGetError() != GL_NO_ERROR; i++) refused = 1;
  return refused;
}

enum { kTestGray = 18, kTestGrayAsSrgb = 75 };

static int gles_window_reads(int gray) {
  GLubyte pixel[4] = {0, 0, 0, 0};
  glClearColor((float)gray / 255.0f, (float)gray / 255.0f, (float)gray / 255.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
  return pixel[0] == pixel[1] && pixel[1] == pixel[2] ? pixel[0] : -1;
}

static int gles_encoded(int read) {
  return read > kTestGrayAsSrgb - 4 && read < kTestGrayAsSrgb + 4;
}

void gles_window_current(void) {
  if (g_window_checked) return;
  g_window_checked = 1;
  GLint encoding = GL_LINEAR;
  gles_refused();
  glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_BACK, GL_FRAMEBUFFER_ATTACHMENT_COLOR_ENCODING, &encoding);
  if (gles_refused())
    glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_BACK_LEFT, GL_FRAMEBUFFER_ATTACHMENT_COLOR_ENCODING, &encoding);
  const int readable = gles_window_reads(255) == 255;
  const int before = readable ? gles_window_reads(kTestGray) : -1;
  if (encoding == GL_SRGB || gles_encoded(before)) {
    gles_refused();
    glDisable(GL_FRAMEBUFFER_SRGB_EXT);
    const int refused = gles_refused();
    const int after = readable ? gles_window_reads(kTestGray) : -1;
    g_window_srgb = !refused;
    fprintf(stderr,
            g_window_srgb && !gles_encoded(after)
                ? "[gles] the driver made the window sRGB, frames go to it as they are (%d read back as %d, %d now)\n"
                : "[gles] the driver made the window sRGB and can't turn that off, colors will look washed out (%d read back "
                  "as %d, %d now)\n",
            kTestGray, before, after);
  }
  glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  gles_refused();
}

void gles_bind_framebuffer(void (BRIDGE_MS* real)(GLenum, GLuint), GLenum target, GLuint framebuffer) {
  real(target, framebuffer);
  if (!g_window_srgb || target == GL_READ_FRAMEBUFFER) return;
  if (framebuffer) glEnable(GL_FRAMEBUFFER_SRGB_EXT);
  else glDisable(GL_FRAMEBUFFER_SRGB_EXT);
}

void gles_delete_framebuffers(void (BRIDGE_MS* real)(GLsizei, const GLuint*), GLsizei n, const GLuint* framebuffers) {
  real(n, framebuffers);
  if (!g_window_srgb) return;
  GLint bound = 0;
  glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &bound);
  if (!bound) glDisable(GL_FRAMEBUFFER_SRGB_EXT);
}

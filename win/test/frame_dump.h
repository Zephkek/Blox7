#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef void (BRIDGE_MS* dump_geti_fn)(unsigned int, int*);
typedef void (BRIDGE_MS* dump_uint_fn)(unsigned int, unsigned int);
typedef void (BRIDGE_MS* dump_enum_fn)(unsigned int);
typedef void (BRIDGE_MS* dump_store_fn)(unsigned int, int);
typedef void (BRIDGE_MS* dump_read_fn)(int, int, int, int, unsigned int, unsigned int, void*);
typedef bool (BRIDGE_MS* dump_size_fn)(SDL_Window*, int*, int*);

static double dump_now(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void dump_write_bmp(const char* path, const unsigned char* rgba, int w, int h) {
  FILE* f = fopen(path, "wb");
  if (!f) return;
  unsigned int size = 54 + (unsigned int)w * h * 4;
  unsigned char head[54] = {'B', 'M'};
  memcpy(head + 2, &size, 4);
  unsigned int v = 54;
  memcpy(head + 10, &v, 4);
  v = 40;
  memcpy(head + 14, &v, 4);
  memcpy(head + 18, &w, 4);
  memcpy(head + 22, &h, 4);
  head[26] = 1;
  head[28] = 32;
  fwrite(head, 1, 54, f);
  unsigned char* row = malloc((size_t)w * 4);
  for (int y = 0; y < h; y++) {
    const unsigned char* src = rgba + (size_t)y * w * 4;
    for (int x = 0; x < w; x++) {
      row[x * 4] = src[x * 4 + 2];
      row[x * 4 + 1] = src[x * 4 + 1];
      row[x * 4 + 2] = src[x * 4];
      row[x * 4 + 3] = 255;
    }
    fwrite(row, 1, (size_t)w * 4, f);
  }
  free(row);
  fclose(f);
}

static void frame_dump(SDL_Window* window, void* (*gl)(const char*)) {
  static int state, count, next;
  static double start, at[32];
  static char dir[512];
  if (state < 0) return;
  if (state == 0) {
    const char* d = getenv("BLOX7_FRAME_DUMP");
    const char* times = getenv("BLOX7_FRAME_DUMP_AT");
    if (!d || !*d || !times) {
      state = -1;
      return;
    }
    snprintf(dir, sizeof dir, "%s", d);
    for (const char* p = times; *p && count < 32;) {
      at[count++] = strtod(p, (char**)&p);
      while (*p == ',' || *p == ' ') p++;
    }
    start = dump_now();
    state = 1;
  }
  if (next >= count || dump_now() - start < at[next]) return;
  int w = 0, h = 0;
  ((dump_size_fn)real("SDL_GetWindowSizeInPixels"))(window, &w, &h);
  dump_geti_fn geti = (dump_geti_fn)gl("glGetIntegerv");
  dump_uint_fn bind_fb = (dump_uint_fn)gl("glBindFramebuffer");
  dump_uint_fn bind_buf = (dump_uint_fn)gl("glBindBuffer");
  dump_enum_fn read_buffer = (dump_enum_fn)gl("glReadBuffer");
  dump_store_fn store = (dump_store_fn)gl("glPixelStorei");
  dump_read_fn read = (dump_read_fn)gl("glReadPixels");
  if (w <= 0 || h <= 0 || !geti || !bind_fb || !bind_buf || !read_buffer || !store || !read) {
    state = -1;
    return;
  }
  int fb = 0, rb = 0, pbo = 0, align = 4, len = 0, skip_rows = 0, skip_pixels = 0;
  geti(0x8CAA, &fb);
  geti(0x0C02, &rb);
  geti(0x88ED, &pbo);
  geti(0x0D05, &align);
  geti(0x0D02, &len);
  geti(0x0D03, &skip_rows);
  geti(0x0D04, &skip_pixels);
  bind_fb(0x8CA8, 0);
  read_buffer(0x0405);
  bind_buf(0x88EB, 0);
  store(0x0D05, 4);
  store(0x0D02, 0);
  store(0x0D03, 0);
  store(0x0D04, 0);
  unsigned char* pixels = malloc((size_t)w * h * 4);
  read(0, 0, w, h, 0x1908, 0x1401, pixels);
  store(0x0D05, align);
  store(0x0D02, len);
  store(0x0D03, skip_rows);
  store(0x0D04, skip_pixels);
  bind_buf(0x88EB, (unsigned int)pbo);
  bind_fb(0x8CA8, (unsigned int)fb);
  read_buffer((unsigned int)rb);
  char path[600];
  snprintf(path, sizeof path, "%s/frame-%03d.bmp", dir, (int)at[next]);
  dump_write_bmp(path, pixels, w, h);
  free(pixels);
  next++;
}

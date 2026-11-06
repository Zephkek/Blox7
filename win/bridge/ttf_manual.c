#include "bridge_common.h"

extern __attribute__((visibility("hidden"))) void* bridge_module_kEntries;

static void* real(const char* name) { return __w32_pe_symbol(bridge_module_kEntries, name); }

BRIDGE_EXPORT TTF_Font* TTF_OpenFont(const char* file, float ptsize) {
  static TTF_Font* (BRIDGE_MS *open_font)(const char*, float);
  if (!open_font) open_font = (TTF_Font* (BRIDGE_MS*)(const char*, float))real("TTF_OpenFont");
  char path[1024];
  if (!file || __w32_win32_path(file, path, sizeof path) != 0) return open_font(file, ptsize);
  return open_font(path, ptsize);
}

#pragma once

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vulkan/vulkan_core.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <SDL3_ttf/SDL_ttf.h>
#include <SDL3_ttf/SDL_textengine.h>

#define BRIDGE_MS __attribute__((ms_abi))
#define BRIDGE_EXPORT __attribute__((visibility("default")))

#ifdef __cplusplus
extern "C" {
#endif

void* __w32_attach_thread(void);
void __w32_frame_beat(void);
void* __w32_pe_open(const char* utf8_path);
void* __w32_pe_symbol(void* module, const char* name);
const char* __w32_exe_dir(void);
int __w32_win32_path(const char* path, char* out, size_t capacity);

struct bridge_entry {
  const char* name;
  void** slot;
};

struct bridge_context {
  void* fn;
  void* userdata;
};

static inline void* bridge_context(void* fn, void* userdata) {
  enum { kSlots = 1024 };
  static struct bridge_context table[kSlots];
  static int used;
  static volatile int lock;
  while (__atomic_exchange_n(&lock, 1, __ATOMIC_ACQUIRE)) {}
  for (int i = 0; i < used; i++) {
    if (table[i].fn == fn && table[i].userdata == userdata) {
      __atomic_store_n(&lock, 0, __ATOMIC_RELEASE);
      return &table[i];
    }
  }
  if (used == kSlots) abort();
  table[used].fn = fn;
  table[used].userdata = userdata;
  void* c = &table[used++];
  __atomic_store_n(&lock, 0, __ATOMIC_RELEASE);
  return c;
}

static inline void bridge_attach(void) { __w32_attach_thread(); }

static inline void* bridge_load(const char* dll) {
  char path[1024];
  snprintf(path, sizeof path, "%s%s", __w32_exe_dir(), dll);
  void* m = __w32_pe_open(path);
  if (!m) {
    fprintf(stderr, "bridge: cannot load %s\n", path);
    abort();
  }
  return m;
}

static inline void bridge_resolve(void* module, const struct bridge_entry* entries, size_t count, const char* dll) {
  int missing = 0;
  for (size_t i = 0; i < count; i++) {
    *entries[i].slot = __w32_pe_symbol(module, entries[i].name);
    if (!*entries[i].slot && missing++ < 8) fprintf(stderr, "bridge: %s has no %s\n", dll, entries[i].name);
  }
}

#define BRIDGE_INIT(dll, entries)                                                          \
  __attribute__((visibility("hidden"))) void* bridge_module_##entries;                    \
  __attribute__((constructor(101))) static void bridge_init_##entries(void) {             \
    bridge_module_##entries = bridge_load(dll);                                            \
    bridge_resolve(bridge_module_##entries, entries, sizeof(entries) / sizeof(entries[0]), dll); \
  }

#ifdef __cplusplus
}
#endif

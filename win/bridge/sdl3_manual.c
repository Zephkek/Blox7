#include <stdarg.h>

#include "bridge_common.h"
#include "vulkan_bridge.h"

extern __attribute__((visibility("hidden"))) void* bridge_module_kEntries;

void gles_bridge_resolve(void* (*lookup)(const char*));
void* gles_bridge_resolve_export(const char* name);

static void* real(const char* name) { return __w32_pe_symbol(bridge_module_kEntries, name); }

static const char* windows_path(const char* file, char* buffer, size_t capacity) {
  return file && __w32_win32_path(file, buffer, capacity) == 0 ? buffer : file;
}

BRIDGE_EXPORT SDL_IOStream* SDL_IOFromFile(const char* file, const char* mode) {
  static SDL_IOStream* (BRIDGE_MS *from_file)(const char*, const char*);
  if (!from_file) from_file = (SDL_IOStream* (BRIDGE_MS*)(const char*, const char*))real("SDL_IOFromFile");
  char path[1024];
  return from_file(windows_path(file, path, sizeof path), mode);
}

BRIDGE_EXPORT void* SDL_LoadFile(const char* file, size_t* datasize) {
  static void* (BRIDGE_MS *load_file)(const char*, size_t*);
  if (!load_file) load_file = (void* (BRIDGE_MS*)(const char*, size_t*))real("SDL_LoadFile");
  char path[1024];
  return load_file(windows_path(file, path, sizeof path), datasize);
}

BRIDGE_EXPORT bool SDL_SetErrorV(const char* fmt, va_list ap) {
  static bool (BRIDGE_MS *set_error)(const char*, ...);
  if (!set_error) set_error = (bool (BRIDGE_MS*)(const char*, ...))real("SDL_SetError");
  char text[1024];
  vsnprintf(text, sizeof text, fmt, ap);
  return set_error("%s", text);
}

BRIDGE_EXPORT bool SDL_SetError(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  bool r = SDL_SetErrorV(fmt, ap);
  va_end(ap);
  return r;
}

BRIDGE_EXPORT void SDL_LogMessageV(int category, SDL_LogPriority priority, const char* fmt, va_list ap) {
  static void (BRIDGE_MS *log_message)(int, SDL_LogPriority, const char*, ...);
  if (!log_message) log_message = (void (BRIDGE_MS*)(int, SDL_LogPriority, const char*, ...))real("SDL_LogMessage");
  char text[4096];
  vsnprintf(text, sizeof text, fmt, ap);
  log_message(category, priority, "%s", text);
}

BRIDGE_EXPORT void SDL_LogMessage(int category, SDL_LogPriority priority, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  SDL_LogMessageV(category, priority, fmt, ap);
  va_end(ap);
}

#define LOG_AT(name, priority)                                        \
  BRIDGE_EXPORT void name(int category, const char* fmt, ...) {       \
    va_list ap;                                                       \
    va_start(ap, fmt);                                                \
    SDL_LogMessageV(category, priority, fmt, ap);                     \
    va_end(ap);                                                       \
  }
LOG_AT(SDL_LogTrace, SDL_LOG_PRIORITY_TRACE)
LOG_AT(SDL_LogVerbose, SDL_LOG_PRIORITY_VERBOSE)
LOG_AT(SDL_LogDebug, SDL_LOG_PRIORITY_DEBUG)
LOG_AT(SDL_LogInfo, SDL_LOG_PRIORITY_INFO)
LOG_AT(SDL_LogWarn, SDL_LOG_PRIORITY_WARN)
LOG_AT(SDL_LogError, SDL_LOG_PRIORITY_ERROR)
LOG_AT(SDL_LogCritical, SDL_LOG_PRIORITY_CRITICAL)

BRIDGE_EXPORT void SDL_Log(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  SDL_LogMessageV(SDL_LOG_CATEGORY_APPLICATION, SDL_LOG_PRIORITY_INFO, fmt, ap);
  va_end(ap);
}

BRIDGE_EXPORT int SDL_vsnprintf(char* text, size_t maxlen, const char* fmt, va_list ap) { return vsnprintf(text, maxlen, fmt, ap); }

BRIDGE_EXPORT int SDL_snprintf(char* text, size_t maxlen, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int r = vsnprintf(text, maxlen, fmt, ap);
  va_end(ap);
  return r;
}

BRIDGE_EXPORT int SDL_vasprintf(char** strp, const char* fmt, va_list ap) {
  // sdl frees this with its own SDL_free so it has to live in sdl's heap
  char* local = NULL;
  int n = vasprintf(&local, fmt, ap);
  if (n < 0) return n;
  *strp = (char*)SDL_malloc((size_t)n + 1);
  if (!*strp) {
    free(local);
    return -1;
  }
  memcpy(*strp, local, (size_t)n + 1);
  free(local);
  return n;
}

BRIDGE_EXPORT int SDL_asprintf(char** strp, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int r = SDL_vasprintf(strp, fmt, ap);
  va_end(ap);
  return r;
}

BRIDGE_EXPORT int SDL_vsscanf(const char* text, const char* fmt, va_list ap) { return vsscanf(text, fmt, ap); }

BRIDGE_EXPORT int SDL_sscanf(const char* text, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int r = vsscanf(text, fmt, ap);
  va_end(ap);
  return r;
}

BRIDGE_EXPORT void SDL_qsort(void* base, size_t nmemb, size_t size, SDL_CompareCallback compare) {
  qsort(base, nmemb, size, compare);
}

BRIDGE_EXPORT void* SDL_bsearch(const void* key, const void* base, size_t nmemb, size_t size, SDL_CompareCallback compare) {
  return bsearch(key, base, nmemb, size, compare);
}

static void* (BRIDGE_MS* g_real_gl_get_proc)(const char*);

static void* lookup_gl(const char* name) {
  if (!g_real_gl_get_proc) g_real_gl_get_proc = (void* (BRIDGE_MS*)(const char*))real("SDL_GL_GetProcAddress");
  return g_real_gl_get_proc(name);
}

BRIDGE_EXPORT SDL_GLContext SDL_GL_CreateContext(SDL_Window* window) {
  static SDL_GLContext (BRIDGE_MS *create)(SDL_Window*);
  static int resolved;
  if (!create) create = (SDL_GLContext (BRIDGE_MS*)(SDL_Window*))real("SDL_GL_CreateContext");
  SDL_GLContext context = create(window);
  if (context && !resolved) {
    gles_bridge_resolve(lookup_gl);
    resolved = 1;
  }
  return context;
}

BRIDGE_EXPORT bool SDL_GL_MakeCurrent(SDL_Window* window, SDL_GLContext context) {
  static bool (BRIDGE_MS *make_current)(SDL_Window*, SDL_GLContext);
  if (!make_current) make_current = (bool (BRIDGE_MS*)(SDL_Window*, SDL_GLContext))real("SDL_GL_MakeCurrent");
  return make_current(window, context);
}

BRIDGE_EXPORT bool SDL_GL_SwapWindow(SDL_Window* window) {
  static bool (BRIDGE_MS *swap)(SDL_Window*);
  if (!swap) swap = (bool (BRIDGE_MS*)(SDL_Window*))real("SDL_GL_SwapWindow");
  const bool ok = swap(window);
  __w32_frame_beat();
  return ok;
}

BRIDGE_EXPORT SDL_FunctionPointer SDL_GL_GetProcAddress(const char* name) {
  if (!lookup_gl(name)) return NULL;
  return (SDL_FunctionPointer)gles_bridge_resolve_export(name);
}

BRIDGE_EXPORT SDL_FunctionPointer SDL_EGL_GetProcAddress(const char* name) {
  (void)name;
  return NULL;
}

BRIDGE_EXPORT bool SDL_Vulkan_LoadLibrary(const char* path) {
  static bool (BRIDGE_MS *load_library)(const char*);
  if (!load_library) load_library = (bool (BRIDGE_MS*)(const char*))real("SDL_Vulkan_LoadLibrary");
  char windows[1024];
  return load_library(windows_path(path, windows, sizeof windows));
}

BRIDGE_EXPORT SDL_FunctionPointer SDL_Vulkan_GetVkGetInstanceProcAddr(void) {
  static void* (BRIDGE_MS *get_proc)(void);
  if (!get_proc) get_proc = (void* (BRIDGE_MS*)(void))real("SDL_Vulkan_GetVkGetInstanceProcAddr");
  void* loader = get_proc();
  return loader ? (SDL_FunctionPointer)vkb_install(loader) : NULL;
}

BRIDGE_EXPORT bool SDL_Vulkan_CreateSurface(SDL_Window* window, VkInstance instance,
                                            const struct VkAllocationCallbacks* allocator, VkSurfaceKHR* surface) {
  static bool (BRIDGE_MS *create)(SDL_Window*, VkInstance, const struct VkAllocationCallbacks*, VkSurfaceKHR*);
  if (!create)
    create = (bool (BRIDGE_MS*)(SDL_Window*, VkInstance, const struct VkAllocationCallbacks*, VkSurfaceKHR*))real(
        "SDL_Vulkan_CreateSurface");
  return create(window, instance, vkb_allocator(allocator), surface);
}

BRIDGE_EXPORT void SDL_Vulkan_DestroySurface(VkInstance instance, VkSurfaceKHR surface,
                                             const struct VkAllocationCallbacks* allocator) {
  static void (BRIDGE_MS *destroy)(VkInstance, VkSurfaceKHR, const struct VkAllocationCallbacks*);
  if (!destroy)
    destroy = (void (BRIDGE_MS*)(VkInstance, VkSurfaceKHR, const struct VkAllocationCallbacks*))real(
        "SDL_Vulkan_DestroySurface");
  destroy(instance, surface, vkb_allocator(allocator));
}

BRIDGE_EXPORT bool SDL_SetMemoryFunctions(SDL_malloc_func m, SDL_calloc_func c, SDL_realloc_func r, SDL_free_func f) {
  (void)m; (void)c; (void)r; (void)f;
  return SDL_SetError("SDL_SetMemoryFunctions is not available in the Windows port");
}

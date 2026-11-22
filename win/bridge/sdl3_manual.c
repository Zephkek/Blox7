#include <dirent.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <unistd.h>

#include "bridge_common.h"
#include "vulkan_bridge.h"

extern __attribute__((visibility("hidden"))) void* bridge_module_kEntries;

void gles_bridge_resolve(void* (*lookup)(const char*));
void* gles_bridge_resolve_export(const char* name);
void gles_window_current(void);
void gles_window_reset(void);

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
  static void* implementation;
  if (!create) create = (SDL_GLContext (BRIDGE_MS*)(SDL_Window*))real("SDL_GL_CreateContext");
  SDL_GLContext context = create(window);
  if (context) {
    void* get_string = lookup_gl("glGetString");
    if (!implementation || get_string != implementation) {
      implementation = get_string;
      gles_bridge_resolve(lookup_gl);
      gles_window_reset();
    }
  }
  return context;
}

BRIDGE_EXPORT bool SDL_GL_MakeCurrent(SDL_Window* window, SDL_GLContext context) {
  static bool (BRIDGE_MS *make_current)(SDL_Window*, SDL_GLContext);
  if (!make_current) make_current = (bool (BRIDGE_MS*)(SDL_Window*, SDL_GLContext))real("SDL_GL_MakeCurrent");
  const bool ok = make_current(window, context);
  if (ok && context) gles_window_current();
  return ok;
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

static struct {
  SDL_EGLAttribArrayCallback platform;
  SDL_EGLIntArrayCallback surface, context;
  void* userdata;
} g_egl;

static BRIDGE_MS SDL_EGLAttrib* egl_platform(void* unused) {
  (void)unused;
  bridge_attach();
  return g_egl.platform(g_egl.userdata);
}

static BRIDGE_MS SDL_EGLint* egl_surface(void* unused, SDL_EGLDisplay display, SDL_EGLConfig config) {
  (void)unused;
  bridge_attach();
  return g_egl.surface(g_egl.userdata, display, config);
}

static BRIDGE_MS SDL_EGLint* egl_context(void* unused, SDL_EGLDisplay display, SDL_EGLConfig config) {
  (void)unused;
  bridge_attach();
  return g_egl.context(g_egl.userdata, display, config);
}

BRIDGE_EXPORT void SDL_EGL_SetAttributeCallbacks(SDL_EGLAttribArrayCallback platform, SDL_EGLIntArrayCallback surface,
                                                 SDL_EGLIntArrayCallback context, void* userdata) {
  static void (BRIDGE_MS *set)(void*, void*, void*, void*);
  if (!set) set = (void (BRIDGE_MS*)(void*, void*, void*, void*))real("SDL_EGL_SetAttributeCallbacks");
  g_egl.platform = platform;
  g_egl.surface = surface;
  g_egl.context = context;
  g_egl.userdata = userdata;
  set(platform ? (void*)egl_platform : NULL, surface ? (void*)egl_surface : NULL, context ? (void*)egl_context : NULL, NULL);
}

typedef long long blob_size;
typedef void (BRIDGE_MS* blob_set_fn)(const void*, blob_size, const void*, blob_size);
typedef blob_size (BRIDGE_MS* blob_get_fn)(const void*, blob_size, void*, blob_size);

static char g_blob_dir[768];

static int blob_path(const void* key, blob_size key_size, char* out, size_t capacity) {
  const unsigned char* k = (const unsigned char*)key;
  if (key_size <= 0 || key_size > 64) return 0;
  size_t used = (size_t)snprintf(out, capacity, "%s/", g_blob_dir);
  if (used + (size_t)key_size * 2 + 1 > capacity) return 0;
  for (blob_size i = 0; i < key_size; i++) used += (size_t)snprintf(out + used, capacity - used, "%02x", k[i]);
  return 1;
}

static BRIDGE_MS void blob_set(const void* key, blob_size key_size, const void* value, blob_size value_size) {
  bridge_attach();
  static int serial;
  char path[1024], temp[1100];
  if (value_size <= 0 || !blob_path(key, key_size, path, sizeof path)) return;
  snprintf(temp, sizeof temp, "%s.%d.%d.tmp", path, (int)getpid(), __atomic_fetch_add(&serial, 1, __ATOMIC_RELAXED));
  FILE* f = fopen(temp, "wb");
  if (!f) return;
  const int ok = fwrite(value, 1, (size_t)value_size, f) == (size_t)value_size;
  if (fclose(f) != 0 || !ok || rename(temp, path) != 0) remove(temp);
}

static BRIDGE_MS blob_size blob_get(const void* key, blob_size key_size, void* value, blob_size value_size) {
  bridge_attach();
  char path[1024];
  if (!blob_path(key, key_size, path, sizeof path)) return 0;
  FILE* f = fopen(path, "rb");
  if (!f) return 0;
  blob_size size = 0;
  if (fseek(f, 0, SEEK_END) == 0) size = (blob_size)ftell(f);
  if (size > 0 && value && value_size >= size &&
      (fseek(f, 0, SEEK_SET) != 0 || fread(value, 1, (size_t)size, f) != (size_t)size))
    size = 0;
  fclose(f);
  return size > 0 ? size : 0;
}

static void blob_trim(const char* dir, long long limit) {
  DIR* d = opendir(dir);
  if (!d) return;
  long long total = 0;
  struct dirent* e;
  char path[1024];
  while ((e = readdir(d))) {
    struct stat st;
    snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
    if (e->d_name[0] != '.' && stat(path, &st) == 0 && S_ISREG(st.st_mode)) total += st.st_size;
  }
  if (total > limit) {
    rewinddir(d);
    while ((e = readdir(d))) {
      snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
      if (e->d_name[0] != '.') remove(path);
    }
  }
  closedir(d);
}

BRIDGE_EXPORT int blox7_angle_blob_cache(const char* dir) {
  static void* (BRIDGE_MS *current_display)(void);
  static void* (BRIDGE_MS *egl_proc)(const char*);
  if (!current_display) current_display = (void* (BRIDGE_MS*)(void))real("SDL_EGL_GetCurrentDisplay");
  if (!egl_proc) egl_proc = (void* (BRIDGE_MS*)(const char*))real("SDL_EGL_GetProcAddress");
  if (!dir || strlen(dir) >= sizeof g_blob_dir || !current_display || !egl_proc) return 0;
  void* display = current_display();
  const char* (BRIDGE_MS *query)(void*, int) = (const char* (BRIDGE_MS*)(void*, int))egl_proc("eglQueryString");
  void (BRIDGE_MS *set_funcs)(void*, blob_set_fn, blob_get_fn) =
      (void (BRIDGE_MS*)(void*, blob_set_fn, blob_get_fn))egl_proc("eglSetBlobCacheFuncsANDROID");
  const char* extensions = display && query ? query(display, 0x3055) : NULL;
  if (!set_funcs || !extensions || !strstr(extensions, "EGL_ANDROID_blob_cache")) return 0;
  char partial[sizeof g_blob_dir];
  for (size_t i = 1; dir[i - 1]; i++) {
    if (dir[i] == '/' || dir[i] == '\0') {
      memcpy(partial, dir, i);
      partial[i] = '\0';
      mkdir(partial, 0755);
    }
  }
  struct stat st;
  if (stat(dir, &st) != 0 || !S_ISDIR(st.st_mode)) return 0;
  blob_trim(dir, 96ll << 20);
  snprintf(g_blob_dir, sizeof g_blob_dir, "%s", dir);
  set_funcs(display, blob_set, blob_get);
  return 1;
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

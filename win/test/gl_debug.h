#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void (BRIDGE_MS* gldbg_cb_fn)(unsigned int, unsigned int, unsigned int, unsigned int, int, const char*, const void*);
typedef void (BRIDGE_MS* gldbg_set_fn)(gldbg_cb_fn, const void*);
typedef void (BRIDGE_MS* gldbg_enable_fn)(unsigned int);
typedef void (BRIDGE_MS* gldbg_control_fn)(unsigned int, unsigned int, unsigned int, int, const unsigned int*, unsigned char);
typedef bool (BRIDGE_MS* gldbg_attr_fn)(int, int);

#define GLDBG_SLOTS 512

static struct {
  unsigned int id, source, type, severity;
  unsigned long long count;
  char text[200];
} g_gldbg[GLDBG_SLOTS];
static int g_gldbg_used;
static FILE* g_gldbg_file;
static unsigned long long g_gldbg_total;

static void BRIDGE_MS gldbg_callback(unsigned int source, unsigned int type, unsigned int id, unsigned int severity,
                                     int length, const char* message, const void* user) {
  (void)user;
  (void)length;
  g_gldbg_total++;
  for (int i = 0; i < g_gldbg_used; i++) {
    if (g_gldbg[i].id == id && g_gldbg[i].type == type && !strncmp(g_gldbg[i].text, message, 80)) {
      unsigned long long c = ++g_gldbg[i].count;
      if (g_gldbg_file && (c == 10 || c == 100 || c == 1000 || c == 10000 || c == 100000)) {
        fprintf(g_gldbg_file, "repeat %llu x id %u: %.120s\n", c, id, g_gldbg[i].text);
        fflush(g_gldbg_file);
      }
      return;
    }
  }
  if (g_gldbg_used < GLDBG_SLOTS) {
    int i = g_gldbg_used++;
    g_gldbg[i].id = id;
    g_gldbg[i].source = source;
    g_gldbg[i].type = type;
    g_gldbg[i].severity = severity;
    g_gldbg[i].count = 1;
    snprintf(g_gldbg[i].text, sizeof g_gldbg[i].text, "%s", message ? message : "");
  }
  if (g_gldbg_file) {
    fprintf(g_gldbg_file, "new source %#x type %#x id %u severity %#x: %s\n", source, type, id, severity,
            message ? message : "");
    fflush(g_gldbg_file);
  }
}

static void gldbg_before_create(void) {
  const char* path = getenv("BLOX7_GL_DEBUG");
  if (!path || !*path) return;
  ((gldbg_attr_fn)real("SDL_GL_SetAttribute"))(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_DEBUG_FLAG);
}

static void gldbg_after_current(void* (*gl)(const char*)) {
  static void* installed;
  const char* path = getenv("BLOX7_GL_DEBUG");
  if (!path || !*path) return;
  void* set = gl("glDebugMessageCallback");
  if (!set) set = gl("glDebugMessageCallbackKHR");
  if (!set) fprintf(stderr, "  [gldebug] no glDebugMessageCallback\n");
  if (!set || set == installed) return;
  installed = set;
  if (!g_gldbg_file) g_gldbg_file = fopen(path, "w");
  fprintf(stderr, "  [gldebug] callback on, log %s %s\n", path, g_gldbg_file ? "open" : "NOT OPEN");
  gldbg_enable_fn enable = (gldbg_enable_fn)gl("glEnable");
  gldbg_control_fn control = (gldbg_control_fn)gl("glDebugMessageControl");
  if (!control) control = (gldbg_control_fn)gl("glDebugMessageControlKHR");
  enable(0x92E0);
  enable(0x8242);
  if (control) control(0x1100, 0x1100, 0x1100, 0, NULL, 1);
  ((gldbg_set_fn)set)(gldbg_callback, NULL);
  if (g_gldbg_file) {
    fprintf(g_gldbg_file, "callback installed\n");
    fflush(g_gldbg_file);
  }
}

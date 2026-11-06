#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>

typedef long (*Fn)(const char*, char*, long, long*);
static Fn g_fn;
static const char* g_path;

static void* run(void* name) {
  char buf[64];
  long fs = 0;
  long got = g_fn(g_path, buf, sizeof buf, &fs);
  printf("%s thread: read %ld bytes, fstatfs type 0x%lx\n", (const char*)name, got, fs);
  return NULL;
}

int main(int argc, char** argv) {
  void* lib = dlopen(argv[1], RTLD_NOW);
  if (!lib) { printf("dlopen: %s\n", dlerror()); return 1; }
  g_fn = (Fn)dlsym(lib, "fake_open_stat_read");
  g_path = argv[2];
  run("main");
  pthread_t t;
  pthread_create(&t, NULL, run, "new");
  pthread_join(t, NULL);
  return 0;
}

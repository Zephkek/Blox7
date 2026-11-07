#include <elf.h>
#include <errno.h>
#include <link.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/auxv.h>

#include "libc_init_common.h"
#include "private/KernelArgumentBlock.h"
#include "private/bionic_elf_tls.h"
#include "private/bionic_globals.h"
#include "private/bionic_tls.h"
#include "pthread_internal.h"
#include "w32_kernel.h"

extern "C" int __cxa_atexit(void (*)(void*), void*, void*);

void w32_init_time();
void w32_init_files();
void w32_init_timezone();
void w32_init_clock_source();
void w32_init_signals();
void w32_init_loader();
void w32_init_net();
void* w32_load_program(const char* path, char* error, size_t error_size);

extern "C" __LIBC_HIDDEN__ void (*__init_array_start[])(int, char**, char**);
extern "C" __LIBC_HIDDEN__ void (*__init_array_end[])(int, char**, char**);
extern "C" __LIBC_HIDDEN__ void (*__fini_array_start[])();
extern "C" __LIBC_HIDDEN__ void (*__fini_array_end[])();

char w32_program[W32_PATH_MAX];

const uintptr_t* w32_auxv;

namespace {

uint8_t g_at_random[16];

uintptr_t* build_args(struct w32boot* boot) {
  WCHAR* env = w32.GetEnvironmentStringsW();
  size_t env_count = 0, env_bytes = 0;
  for (WCHAR* p = env; *p;) {
    size_t n = 0;
    while (p[n]) n++;
    if (p[0] != '=') {  // windows' secret per-drive cwd vars
      env_count++;
      env_bytes += n * 3 + 1;
    }
    p += n + 1;
  }
  constexpr size_t kAux = 12;
  size_t words = 1 + static_cast<size_t>(boot->argc) + 1 + env_count + 1 + 2 * kAux;
  W32_SYSTEM_INFO si;
  w32.GetSystemInfo(&si);
  auto* block = static_cast<uintptr_t*>(w32.VirtualAlloc(nullptr, words * 8 + env_bytes, 0x3000, 0x04));
  if (!block) w32_fatal("out of memory at start-up");
  char* strings = reinterpret_cast<char*>(block + words);
  size_t w = 0;
  block[w++] = static_cast<uintptr_t>(boot->argc);
  for (int i = 0; i < boot->argc; i++) block[w++] = reinterpret_cast<uintptr_t>(boot->argv[i]);
  block[w++] = 0;
  for (WCHAR* p = env; *p;) {
    size_t n = 0;
    while (p[n]) n++;
    if (p[0] != '=') {
      int len = w32_utf16_to_utf8(p, strings, n * 3 + 1);
      if (len >= 0) {
        // windows doesn't care about env var case, android code does
        if (strncasecmp(strings, "BLOX7_", 6) == 0 || strncasecmp(strings, "ROBLOX_", 7) == 0) {
          for (char* c = strings; *c && *c != '='; c++)
            if (*c >= 'a' && *c <= 'z') *c = static_cast<char>(*c - 32);
        }
        block[w++] = reinterpret_cast<uintptr_t>(strings);
        strings += len + 1;
      }
    }
    p += n + 1;
  }
  block[w++] = 0;
  w32.FreeEnvironmentStringsW(env);

  w32.SystemFunction036(g_at_random, sizeof g_at_random);
  unsigned regs[4];
  __asm__("cpuid" : "=a"(regs[0]), "=b"(regs[1]), "=c"(regs[2]), "=d"(regs[3]) : "a"(1), "c"(0));
  const uintptr_t aux[kAux][2] = {
      {AT_PHDR, reinterpret_cast<uintptr_t>(boot->libc_phdr)},
      {AT_PHENT, sizeof(ElfW(Phdr))},
      {AT_PHNUM, boot->libc_phnum},
      {AT_PAGESZ, 4096},
      {AT_BASE, boot->libc_base},
      {AT_HWCAP, regs[3]},
      {AT_HWCAP2, 0},
      {AT_CLKTCK, 100},
      {AT_SECURE, 0},
      {AT_RANDOM, reinterpret_cast<uintptr_t>(g_at_random)},
      {AT_EXECFN, reinterpret_cast<uintptr_t>(boot->argv[0])},
      {AT_NULL, 0},
  };
  w32_auxv = block + w;
  for (auto& a : aux) {
    block[w++] = a[0];
    block[w++] = a[1];
  }
  return block;
}

void call_libc_fini(void*) {
  for (auto* f = __fini_array_end; f > __fini_array_start;) (*--f)();
}

const char* program_path(struct w32boot* boot, int* skip_args) {
  static char path[W32_PATH_MAX];
  *skip_args = 0;
  if (boot->argc >= 3 && strcmp(boot->argv[1], "--w32-run") == 0) {
    *skip_args = 2;
    return boot->argv[2];
  }
  const char* exe = boot->argv[0];
  const char* base = exe;
  for (const char* p = exe; *p; p++)
    if (*p == '\\' || *p == '/') base = p + 1;
  size_t n = strlen(base);
  if (n > 4 && (strcmp(base + n - 4, ".exe") == 0 || strcmp(base + n - 4, ".EXE") == 0)) n -= 4;
  if (n > 8 && strncmp(base + n - 8, "-console", 8) == 0) n -= 8;
  WCHAR dir[W32_PATH_MAX];
  if (w32_utf8_to_utf16(boot->exe_dir, dir, W32_PATH_MAX) < 0 || w32_unix_path(dir, path, sizeof path) < 0)
    w32_fatal("the program's directory has no POSIX path");
  size_t len = strlen(path);
  snprintf(path + len, sizeof path - len, "%s%.*s.so", len && path[len - 1] == '/' ? "" : "/", static_cast<int>(n), base);
  return path;
}

}  // namespace

extern "C" __attribute__((visibility("default"), no_stack_protector)) int __w32_libc_start(struct w32boot* boot) {
  w32_boot = boot;
  w32_init_api();
  w32_init_time();

  bionic_tcb temp_tcb __attribute__((uninitialized));
  __builtin_memset(&temp_tcb, 0, sizeof(temp_tcb));
  uintptr_t* raw_args = build_args(boot);
  KernelArgumentBlock args(raw_args);

  __libc_init_main_thread_early(args, &temp_tcb);
  w32_init_threads();
  w32_debug("start: thread pointer set");
  __libc_init_main_thread_late();
  w32_debug("start: main thread %lu", static_cast<unsigned long>(w32.GetCurrentThreadId()));
  __libc_init_globals();
  __libc_shared_globals()->init_progname = args.argv[0];
  __libc_init_AT_SECURE(args.envp);

  StaticTlsLayout& layout = __libc_shared_globals()->static_tls_layout;
  layout.reserve_bionic_tls();
  layout.reserve_exe_segment_and_tcb(nullptr, args.argv[0]);
  __libc_tls_generation_copy = __libc_shared_globals()->tls_modules.generation;
  layout.finish_layout();

  __libc_init_main_thread_final();
  w32_debug("start: static tls");
  __libc_init_common();
  __libc_init_scudo();
  __libc_globals.mutate(__libc_init_malloc);
  __libc_init_fork_handler();
  w32_thread_attach_fs();
  // app allocator settings. without the decay time scudo frees every big block instantly and roblox hates that
  mallopt(M_BIONIC_ZERO_INIT, 0);
  mallopt(M_DECAY_TIME, 1);
  w32_debug("start: malloc ready");

  for (auto* f = __init_array_start; f < __init_array_end; f++) (*f)(args.argc, args.argv, args.envp);
  __cxa_atexit(call_libc_fini, nullptr, nullptr);

  w32_init_net();
  w32_init_files();
  if (const char* cwd = getenv("BLOX7_W32_CWD")) {
    chdir(cwd);
    unsetenv("BLOX7_W32_CWD");
  }
  w32_init_timezone();
  w32_init_clock_source();
  w32_init_signals();
  w32_stats_start();
  w32_stall_start();
  w32_init_loader();
  w32_init_vex();
  w32_debug("start: files, signals, loader, vex");

  int skip = 0;
  const char* program = program_path(boot, &skip);
  if (program[0] == '/' || !getcwd(w32_program, W32_PATH_MAX)) {
    snprintf(w32_program, W32_PATH_MAX, "%s", program);
  } else {
    const size_t used = strlen(w32_program);
    snprintf(w32_program + used, W32_PATH_MAX - used, "/%s", program);
  }
  char error[512];
  auto main_fn = reinterpret_cast<int (*)(int, char**, char**)>(w32_load_program(program, error, sizeof error));
  if (!main_fn) {
    char message[1024];
    snprintf(message, sizeof message, "Could not load %s:\n%s", program, error);
    w32_fatal(message);
  }
  w32_debug("start: running %s", program);
  int argc = args.argc - skip;
  char** argv = args.argv + skip;
  argv[0] = args.argv[0];
  exit(main_fn(argc, argv, args.envp));
}

extern "C" __noreturn void __libc_init(void* raw_args, void (*)(void), int (*slingshot)(int, char**, char**),
                                       structors_array_t const* const) {
  KernelArgumentBlock args(raw_args);
  exit(slingshot(args.argc, args.argv, args.envp));
}

static int g_target_sdk_version{__ANDROID_API__};

extern "C" int android_get_application_target_sdk_version() { return g_target_sdk_version; }

extern "C" void android_set_application_target_sdk_version(int target) {
  g_target_sdk_version = target;
  __libc_set_target_sdk_version(target);
}

__LIBC_HIDDEN__ libc_shared_globals* __libc_shared_globals() {
  static libc_shared_globals globals;
  return &globals;
}

#include <dlfcn.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define FORM(name, before, bytes, disp, after)                                                              \
  __asm__(".text\n.p2align 4\n.globl " #name "\n.hidden " #name "\n.type " #name ",@function\n" #name ":\n" \
          ".cfi_startproc\n" before "\n.byte " bytes "\n.long " #disp "\n" after "\nret\n.cfi_endproc\n"    \
          ".size " #name ",.-" #name "\n");                                                                 \
  uint64_t name(uint64_t)

#define BARE(name, disp)                                                                                    \
  __asm__(".text\n.p2align 4\n.globl " #name "\n.hidden " #name "\n.type " #name ",@function\n" #name ":\n" \
          ".byte 0x64,0x48,0x8b,0x04,0x25\n.long " #disp "\nret\n.size " #name ",.-" #name "\n");           \
  uint64_t name(uint64_t)

#define FORMS(d, s)                                                                  \
  BARE(mov_bare_##s, d);                                                             \
  FORM(mov_##s, "", "0x64,0x48,0x8b,0x04,0x25", d, "");                              \
  FORM(mov_r9_##s, "", "0x64,0x4c,0x8b,0x0c,0x25", d, "mov %r9,%rax");               \
  FORM(sub_##s, "", "0x64,0x48,0x2b,0x3c,0x25", d, "mov %rdi,%rax");                 \
  FORM(xor_##s, "", "0x64,0x48,0x33,0x3c,0x25", d, "mov %rdi,%rax");                 \
  FORM(cmp_##s, "", "0x64,0x48,0x3b,0x3c,0x25", d, "sete %al\nmovzbl %al,%eax");     \
  FORM(mov_rexb_##s, "", "0x64,0x49,0x8b,0x04,0x25", d, "");                         \
  FORM(add_##s, "", "0x64,0x48,0x03,0x3c,0x25", d, "mov %rdi,%rax");                 \
  FORM(or_##s, "", "0x64,0x48,0x0b,0x3c,0x25", d, "mov %rdi,%rax");                  \
  FORM(and_##s, "", "0x64,0x48,0x23,0x3c,0x25", d, "mov %rdi,%rax");                 \
  FORM(adc_##s, "clc", "0x64,0x48,0x13,0x3c,0x25", d, "mov %rdi,%rax");              \
  FORM(sbb_##s, "clc", "0x64,0x48,0x1b,0x3c,0x25", d, "mov %rdi,%rax");              \
  FORM(cmp_mem_##s, "", "0x64,0x48,0x39,0x3c,0x25", d, "sete %al\nmovzbl %al,%eax"); \
  FORM(test_##s, "", "0x64,0x48,0x85,0x3c,0x25", d, "setz %al\nmovzbl %al,%eax")

FORMS(0x00, self);
FORMS(0x08, id);
FORMS(0x28, guard);

enum { MOV, SUB, XOR, CMP, ADD, OR, AND, TEST };

struct Form {
  const char* name;
  uint64_t (*fn)(uint64_t);
  int kind, wide;
};

#define TABLE(s)                                                                                                  \
  {                                                                                                               \
    {"mov", mov_##s, MOV, 0}, {"mov r9", mov_r9_##s, MOV, 0}, {"sub", sub_##s, SUB, 0}, {"xor", xor_##s, XOR, 0}, \
    {"cmp", cmp_##s, CMP, 0}, {"mov outside any function", mov_bare_##s, MOV, 0},                                 \
    {"mov rex.b", mov_rexb_##s, MOV, 1}, {"add", add_##s, ADD, 1}, {"or", or_##s, OR, 1},                         \
    {"and", and_##s, AND, 1}, {"adc", adc_##s, ADD, 1}, {"sbb", sbb_##s, SUB, 1},                                 \
    {"cmp mem", cmp_mem_##s, CMP, 1}, {"test", test_##s, TEST, 1},                                                \
  }

#define FORM_COUNT 14
static const struct Form g_forms[3][FORM_COUNT] = {TABLE(self), TABLE(id), TABLE(guard)};
static const char* const g_slot_names[3] = {"fs:0", "fs:8", "fs:0x28"};

static int g_wide;
static long g_checks, g_wrong;
static uintptr_t g_guard;

static void expect(int ok, const char* who, const char* what, const char* form) {
  __atomic_add_fetch(&g_checks, 1, __ATOMIC_RELAXED);
  if (ok) return;
  if (__atomic_add_fetch(&g_wrong, 1, __ATOMIC_RELAXED) <= 8) printf("  wrong on %s: %s %s\n", who, what, form);
}

__attribute__((noinline)) static unsigned work(unsigned seed) {
  volatile char buf[64];
  unsigned h = 0;
  for (int i = 0; i < 64; i++) buf[i] = (char)(seed + i);
  for (int i = 0; i < 64; i++) h = h * 31 + (unsigned char)buf[i];
  return h;
}

static void check(const char* who) {
  const uint64_t probes[4] = {0, 1, 0x0123456789abcdefull, ~0ull};
  uintptr_t want[3];
  void** tls = (void**)mov_self(0);
  expect(tls && tls[0] == (void*)tls, who, "thread pointer", "points at itself");
  if (!tls) return;
  want[0] = (uintptr_t)tls;
  want[1] = (uintptr_t)pthread_self();
  want[2] = (uintptr_t)tls[5];
  expect(tls[1] == (void*)want[1], who, "thread id slot", "is pthread_self");
  expect(want[2] != 0 && want[2] == g_guard, who, "canary", "is the process canary");
  for (int s = 0; s < 3; s++) {
    for (int f = 0; f < FORM_COUNT; f++) {
      const struct Form* form = &g_forms[s][f];
      const uint64_t v = want[s];
      if (form->wide && !g_wide) continue;
      for (int p = 0; p < 6; p++) {
        const uint64_t a = p < 4 ? probes[p] : p == 4 ? v : ~v, got = form->fn(a);
        uint64_t exp = 0;
        switch (form->kind) {
          case MOV: exp = v; break;
          case SUB: exp = a - v; break;
          case XOR: exp = a ^ v; break;
          case ADD: exp = a + v; break;
          case OR: exp = a | v; break;
          case AND: exp = a & v; break;
          case CMP: exp = a == v; break;
          case TEST: exp = (a & v) == 0; break;
        }
        expect(got == exp, who, g_slot_names[s], form->name);
      }
    }
  }
  work((unsigned)want[1]);
}

enum { THREADS = 32, SHORT = 1000 };
static void* g_seen[THREADS + 1];
static int g_started;

static void* busy(void* arg) {
  *(void**)arg = (void*)mov_self(0);
  __atomic_add_fetch(&g_started, 1, __ATOMIC_RELEASE);
  while (__atomic_load_n(&g_started, __ATOMIC_ACQUIRE) < THREADS) usleep(200);
  for (int i = 0; i < 200; i++) {
    check("a thread");
    if (i % 8 == 0) usleep(200);
    else sched_yield();
  }
  return NULL;
}

static void* brief(void* arg) {
  (void)arg;
  check("a short thread");
  return (void*)mov_id(0);
}

typedef uint32_t (__attribute__((ms_abi)) * WinThreadProc)(void*);
typedef void* (__attribute__((ms_abi)) * CreateThreadFn)(void*, size_t, WinThreadProc, void*, uint32_t, uint32_t*);
typedef uint32_t (__attribute__((ms_abi)) * WaitFn)(void*, uint32_t);
typedef int (__attribute__((ms_abi)) * CloseFn)(void*);

static void* (*g_attach)(void);

__attribute__((ms_abi, no_stack_protector)) static uint32_t adopted(void* arg) {
  (void)arg;
  g_attach();
  for (int i = 0; i < 50; i++) {
    check("an adopted Windows thread");
    sched_yield();
  }
  return 0;
}

static int adopt(int count) {
  void* (*pe_open)(const char*) = (void* (*)(const char*))dlsym(RTLD_DEFAULT, "__w32_pe_open");
  void* (*pe_symbol)(void*, const char*) = (void* (*)(void*, const char*))dlsym(RTLD_DEFAULT, "__w32_pe_symbol");
  g_attach = (void* (*)(void))dlsym(RTLD_DEFAULT, "__w32_attach_thread");
  if (!pe_open || !pe_symbol || !g_attach) return 0;
  void* kernel32 = pe_open("kernel32.dll");
  CreateThreadFn create = (CreateThreadFn)pe_symbol(kernel32, "CreateThread");
  WaitFn wait = (WaitFn)pe_symbol(kernel32, "WaitForSingleObject");
  CloseFn close_handle = (CloseFn)pe_symbol(kernel32, "CloseHandle");
  if (!create || !wait || !close_handle) return 0;
  void* handles[16];
  int made = 0;
  for (int i = 0; i < count && i < 16; i++) {
    handles[made] = create(NULL, 0, adopted, NULL, 0, NULL);
    if (handles[made]) made++;
  }
  for (int i = 0; i < made; i++) {
    wait(handles[i], 0xffffffffu);
    close_handle(handles[i]);
  }
  return made;
}

__attribute__((noinline)) static void fill(volatile char* p, int n) {
  for (int i = 0; i < n; i++) p[i] = 0x41;
}

__attribute__((noinline)) static int victim(int n) {
  volatile char buf[16];
  fill(buf, n);
  return buf[0];
}

int main(int argc, char** argv) {
  pthread_t threads[THREADS];
  int distinct = 1;
  setvbuf(stdout, NULL, _IONBF, 0);
  if (argc > 1 && !strcmp(argv[1], "smash")) {
    printf("fsslots: smashing a stack frame, the canary check has to stop this\n");
    return victim(64) + 100;
  }
  g_wide = argc > 1 && !strcmp(argv[1], "wide");
  g_guard = (uintptr_t)((void**)mov_self(0))[5];
  check("the main thread");
  g_seen[THREADS] = (void*)mov_self(0);
  for (int i = 0; i < THREADS; i++) {
    if (pthread_create(&threads[i], NULL, busy, &g_seen[i])) {
      printf("fsslots: could not start thread %d\n", i);
      return 2;
    }
  }
  for (int i = 0; i < THREADS; i++) pthread_join(threads[i], NULL);
  for (int i = 0; i <= THREADS; i++)
    for (int k = 0; k < i; k++)
      if (g_seen[i] == g_seen[k]) distinct = 0;
  expect(distinct, "threads alive together", "thread pointers", "are distinct");
  for (int i = 0; i < SHORT; i++) {
    pthread_t t;
    void* id = NULL;
    if (pthread_create(&t, NULL, brief, NULL)) {
      printf("fsslots: could not start short thread %d\n", i);
      return 2;
    }
    pthread_join(t, &id);
    expect(id == (void*)t, "a short thread", "fs:8", "is the id pthread_create gave");
  }
  const int adopted_threads = adopt(8);
  check("the main thread again");
  printf("fsslots: main, %d threads, %d short threads, %d adopted Windows threads, %s forms: %ld checks, %ld wrong\n",
         THREADS, SHORT, adopted_threads, g_wide ? "all 14" : "the 6 emulated", g_checks, g_wrong);
  return g_wrong != 0;
}

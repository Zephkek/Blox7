#include <errno.h>
#include <sched.h>
#include <stdint.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/sysinfo.h>
#include <sys/utsname.h>

#include "private/icu4x.h"
#include "gwp_asan_wrappers.h"
#include "w32_kernel.h"
#include "w32_syscalls.h"

namespace {

constexpr DWORD CT_CTYPE1 = 1, CT_CTYPE3 = 4;
constexpr uint16_t C1_UPPER = 0x1, C1_LOWER = 0x2, C1_DIGIT = 0x4, C1_SPACE = 0x8, C1_PUNCT = 0x10, C1_CNTRL = 0x20;
constexpr uint16_t C1_BLANK = 0x40, C1_XDIGIT = 0x80, C1_ALPHA = 0x100, C1_DEFINED = 0x200;
constexpr uint16_t C3_NONSPACING = 0x1, C3_FULLWIDTH = 0x80, C3_IDEOGRAPH = 0x100;

uint16_t ctype(uint32_t cp, DWORD which) {
  WCHAR s[2];
  int n = 1;
  if (cp >= 0x10000) {
    if (cp > 0x10ffff) return 0;
    s[0] = static_cast<WCHAR>(0xd800 + ((cp - 0x10000) >> 10));
    s[1] = static_cast<WCHAR>(0xdc00 + ((cp - 0x10000) & 0x3ff));
    n = 2;
  } else {
    s[0] = static_cast<WCHAR>(cp);
  }
  uint16_t t[2] = {0, 0};
  if (!w32.GetStringTypeW(which, s, n, t)) return 0;
  return t[0];
}

uint32_t map_case(uint32_t cp, bool upper) {
  if (cp >= 0x10000 || (cp >= 0xd800 && cp < 0xe000)) return cp;
  WCHAR c = static_cast<WCHAR>(cp);
  if (upper) w32.CharUpperBuffW(&c, 1); else w32.CharLowerBuffW(&c, 1);
  return c;
}

}  // namespace

extern "C" {

uint8_t __icu4x_bionic_general_category(uint32_t cp) {
  uint16_t t1 = ctype(cp, CT_CTYPE1);
  if (t1 & C1_CNTRL) return U_CONTROL_CHAR;
  if (t1 & C1_DIGIT) return U_DECIMAL_NUMBER;
  if (ctype(cp, CT_CTYPE3) & C3_NONSPACING) return U_NON_SPACING_MARK;
  if (t1 & C1_PUNCT) return cp == '-' ? U_DASH_PUNCTUATION : U_OTHER_PUNCTUATION;
  if (cp == 0x200b || cp == 0x200c || cp == 0x200d || cp == 0x2060 || cp == 0xfeff) return U_FORMAT_CHAR;
  return (t1 & C1_DEFINED) ? 1 : 0;
}

uint8_t __icu4x_bionic_east_asian_width(uint32_t cp) {
  uint16_t t3 = ctype(cp, CT_CTYPE3);
  if (t3 & (C3_FULLWIDTH | C3_IDEOGRAPH)) return U_EA_WIDE;
  if ((cp >= 0x1100 && cp <= 0x115f) || (cp >= 0x2e80 && cp <= 0xa4cf) || (cp >= 0xac00 && cp <= 0xd7a3) ||
      (cp >= 0x1f300 && cp <= 0x1faff) || (cp >= 0x20000 && cp <= 0x3fffd))
    return U_EA_WIDE;
  return U_EA_NEUTRAL;
}

uint8_t __icu4x_bionic_hangul_syllable_type(uint32_t cp) {
  if (cp >= 0x1100 && cp <= 0x115f) return U_HST_LEADING_JAMO;
  if (cp >= 0x1160 && cp <= 0x11a7) return U_HST_VOWEL_JAMO;
  if (cp >= 0x11a8 && cp <= 0x11ff) return U_HST_TRAILING_JAMO;
  if (cp >= 0xac00 && cp <= 0xd7a3) return ((cp - 0xac00) % 28) == 0 ? U_HST_LV_SYLLABLE : U_HST_LVT_SYLLABLE;
  return U_HST_NOT_APPLICABLE;
}

bool __icu4x_bionic_is_alphabetic(uint32_t cp) { return ctype(cp, CT_CTYPE1) & C1_ALPHA; }
bool __icu4x_bionic_is_default_ignorable_code_point(uint32_t cp) {
  return cp == 0xad || cp == 0x34f || (cp >= 0x200b && cp <= 0x200f) || (cp >= 0x2060 && cp <= 0x206f) || cp == 0xfeff ||
         (cp >= 0xfe00 && cp <= 0xfe0f) || (cp >= 0xe0000 && cp <= 0xe0fff);
}
bool __icu4x_bionic_is_lowercase(uint32_t cp) { return ctype(cp, CT_CTYPE1) & C1_LOWER; }
bool __icu4x_bionic_is_uppercase(uint32_t cp) { return ctype(cp, CT_CTYPE1) & C1_UPPER; }
bool __icu4x_bionic_is_alnum(uint32_t cp) { return ctype(cp, CT_CTYPE1) & (C1_ALPHA | C1_DIGIT); }
bool __icu4x_bionic_is_blank(uint32_t cp) { return ctype(cp, CT_CTYPE1) & C1_BLANK; }
bool __icu4x_bionic_is_graph(uint32_t cp) {
  uint16_t t = ctype(cp, CT_CTYPE1);
  return (t & C1_DEFINED) && !(t & (C1_SPACE | C1_CNTRL));
}
bool __icu4x_bionic_is_print(uint32_t cp) {
  uint16_t t = ctype(cp, CT_CTYPE1);
  return (t & C1_DEFINED) && !(t & C1_CNTRL);
}
bool __icu4x_bionic_is_xdigit(uint32_t cp) { return ctype(cp, CT_CTYPE1) & C1_XDIGIT; }
bool __icu4x_bionic_is_white_space(uint32_t cp) { return ctype(cp, CT_CTYPE1) & C1_SPACE; }
uint32_t __icu4x_bionic_to_upper(uint32_t ch) { return map_case(ch, true); }
uint32_t __icu4x_bionic_to_lower(uint32_t ch) { return map_case(ch, false); }

}  // extern "C"

bool EnableGwpAsan(const android_mallopt_gwp_asan_options_t&) { return false; }
bool MaybeInitGwpAsanFromLibc(libc_globals*) { return false; }
bool DispatchIsGwpAsan(const MallocDispatch*) { return false; }

long sys_uname(long buf, long, long, long, long, long) {
  auto u = reinterpret_cast<utsname*>(buf);
  memset(u, 0, sizeof(*u));
  strcpy(u->sysname, "Linux");
  strcpy(u->release, "5.15.0-blox7-win32");
  strcpy(u->version, "#1 SMP");
  strcpy(u->machine, "x86_64");
  strcpy(u->domainname, "(none)");
  WCHAR name[64];
  DWORD n = 64;
  if (!w32.GetComputerNameW(name, &n) || w32_utf16_to_utf8(name, u->nodename, sizeof(u->nodename)) < 0)
    strcpy(u->nodename, "localhost");
  return 0;
}

long sys_sysinfo(long buf, long, long, long, long, long) {
  auto s = reinterpret_cast<struct sysinfo*>(buf);
  memset(s, 0, sizeof(*s));
  W32_MEMORYSTATUSEX m{};
  m.length = sizeof m;
  w32.GlobalMemoryStatusEx(&m);
  s->uptime = static_cast<long>(w32.GetTickCount64() / 1000);
  s->totalram = m.total_phys;
  s->freeram = m.avail_phys;
  s->totalswap = m.total_page > m.total_phys ? m.total_page - m.total_phys : 0;
  s->freeswap = m.avail_page > m.avail_phys ? m.avail_page - m.avail_phys : 0;
  s->procs = 1;
  s->mem_unit = 1;
  return 0;
}

long sys_getrandom(long buf, long len, long, long, long, long) {
  size_t n = static_cast<size_t>(len) > 0x10000000 ? 0x10000000 : static_cast<size_t>(len);
  return w32.SystemFunction036(reinterpret_cast<void*>(buf), static_cast<DWORD>(n)) ? static_cast<long>(n) : -EIO;
}

long sys_getcpu(long cpu, long node, long, long, long, long) {
  if (cpu) *reinterpret_cast<unsigned*>(cpu) = w32.GetCurrentProcessorNumber();
  if (node) *reinterpret_cast<unsigned*>(node) = 0;
  return 0;
}

long sys_sched_getaffinity(long, long size, long mask, long, long, long) {
  W32_SYSTEM_INFO si;
  w32.GetSystemInfo(&si);
  size_t bytes = static_cast<size_t>(size) < 128 ? static_cast<size_t>(size) : 128;
  memset(reinterpret_cast<void*>(mask), 0, bytes);
  for (unsigned i = 0; i < si.processor_count && i / 8 < bytes; i++) reinterpret_cast<uint8_t*>(mask)[i / 8] |= 1u << (i % 8);
  return static_cast<long>(bytes);
}

long sys_sched_setaffinity(long, long, long, long, long, long) { return 0; }
long sys_sched_getscheduler(long, long, long, long, long, long) { return SCHED_OTHER; }
long sys_sched_getparam(long, long param, long, long, long, long) {
  *reinterpret_cast<int*>(param) = 0;
  return 0;
}

long sys_getuid(long, long, long, long, long, long) { return 10000; }
long sys_geteuid(long, long, long, long, long, long) { return 10000; }
long sys_getgid(long, long, long, long, long, long) { return 10000; }
long sys_getegid(long, long, long, long, long, long) { return 10000; }
long sys_getresuid(long r, long e, long s, long, long, long) {
  *reinterpret_cast<unsigned*>(r) = *reinterpret_cast<unsigned*>(e) = *reinterpret_cast<unsigned*>(s) = 10000;
  return 0;
}
long sys_getresgid(long r, long e, long s, long, long, long) { return sys_getresuid(r, e, s, 0, 0, 0); }
long sys_getgroups(long, long, long, long, long, long) { return 0; }
long sys_setuid(long, long, long, long, long, long) { return 0; }
long sys_setgid(long, long, long, long, long, long) { return 0; }
long sys_setsid(long, long, long, long, long, long) { return -EPERM; }
long sys_getsid(long, long, long, long, long, long) { return static_cast<long>(w32.GetCurrentProcessId()); }
long sys_getpgid(long, long, long, long, long, long) { return static_cast<long>(w32.GetCurrentProcessId()); }
long sys_setpgid(long, long, long, long, long, long) { return 0; }

long sys_prlimit64(long, long resource, long new_l, long old_l, long, long) {
  struct rl { uint64_t cur, max; };
  if (old_l) {
    auto o = reinterpret_cast<rl*>(old_l);
    switch (resource) {
      case RLIMIT_STACK: o->cur = 8 << 20; o->max = ~0ull; break;
      case RLIMIT_NOFILE: o->cur = o->max = 65536; break;
      default: o->cur = o->max = ~0ull; break;
    }
  }
  (void)new_l;
  return 0;
}
long sys_getrlimit(long resource, long rlim, long, long, long, long) { return sys_prlimit64(0, resource, 0, rlim, 0, 0); }
long sys_setrlimit(long, long, long, long, long, long) { return 0; }

long sys_prctl(long option, long arg2, long arg3, long arg4, long arg5, long) {
  switch (option) {
    case PR_SET_NAME: {
      char* comm = w32_thread_comm();
      strncpy(comm, reinterpret_cast<const char*>(arg2), 15);
      comm[15] = 0;
      w32_note_thread_name(comm);
      return 0;
    }
    case PR_GET_NAME:
      memcpy(reinterpret_cast<char*>(arg2), w32_thread_comm(), 16);
      return 0;
    case PR_SET_VMA:
      if (arg2 != 0) return -EINVAL;
      return w32_mem_set_anon_name(static_cast<uintptr_t>(arg3), static_cast<size_t>(arg4),
                                   reinterpret_cast<const char*>(arg5));
    case PR_SET_DUMPABLE:
    case PR_SET_PDEATHSIG:
    case PR_SET_NO_NEW_PRIVS:
      return 0;
    case PR_GET_DUMPABLE:
      return 1;
    default:
      return -EINVAL;
  }
}

long sys_personality(long, long, long, long, long, long) { return 0; }
long sys_capget(long, long, long, long, long, long) { return 0; }
long sys_capset(long, long, long, long, long, long) { return -EPERM; }
long sys_ptrace(long, long, long, long, long, long) { return -EPERM; }
long sys_arch_prctl(long, long, long, long, long, long) { return -EINVAL; }
long sys_set_robust_list(long, long, long, long, long, long) { return 0; }
long sys_membarrier(long, long, long, long, long, long) { return -ENOSYS; }
long sys_rseq(long, long, long, long, long, long) { return -ENOSYS; }

long sys_clone(long, long, long, long, long, long) { return -ENOSYS; }
long sys_fork(long, long, long, long, long, long) { return -ENOSYS; }
long sys_vfork(long, long, long, long, long, long) { return -ENOSYS; }

extern "C" int __register_atfork(void (*)(), void (*)(), void (*)(), void*);
extern "C" void* __dso_handle;
extern "C" int pthread_atfork(void (*prepare)(), void (*parent)(), void (*child)()) {
  return __register_atfork(prepare, parent, child, &__dso_handle);
}

extern "C" pid_t vfork() {
  errno = ENOSYS;
  return -1;
}

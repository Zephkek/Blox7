// linux's /proc and the /sys bits android reads, made up from what windows says about the process
#include <elf.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include "w32_fd.h"
#include "w32_kernel.h"
#include "w32_syscalls.h"

namespace {

constexpr DWORD MEM_COMMIT = 0x1000, MEM_FREE = 0x10000;
constexpr DWORD MEM_MAPPED = 0x40000, MEM_IMAGE = 0x1000000, MEM_PRIVATE = 0x20000;
constexpr uintptr_t kUserEnd = 0x7FFFFFFF0000;
constexpr unsigned kClockTicks = 100;

struct Text {
  char* data = nullptr;
  size_t size = 0, cap = 0;
  bool grow(size_t more) {
    if (size + more + 1 <= cap) return true;
    size_t want = (size + more + 1) * 2;
    char* p = static_cast<char*>(realloc(data, want));
    if (!p) return false;
    data = p;
    cap = want;
    return true;
  }
  void bytes(const void* p, size_t n) {
    if (!grow(n)) return;
    memcpy(data + size, p, n);
    size += n;
    data[size] = 0;
  }
  void add(const char* fmt, ...) __printflike(2, 3) {
    char small[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(small, sizeof small, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (static_cast<size_t>(n) < sizeof small) {
      bytes(small, static_cast<size_t>(n));
      return;
    }
    if (!grow(static_cast<size_t>(n))) return;
    va_start(ap, fmt);
    vsnprintf(data + size, cap - size, fmt, ap);
    va_end(ap);
    size += static_cast<size_t>(n);
  }
};

bool is(const char* path, const char* name) { return strcmp(path, name) == 0; }

unsigned long pid() { return static_cast<unsigned long>(w32.GetCurrentProcessId()); }


unsigned thread_count() {
  int n = w32_thread_count();
  return n > 0 ? static_cast<unsigned>(n) : 1;
}

struct Times {
  uint64_t user_ticks, system_ticks, start_ticks;
};

uint64_t filetime_100ns(W32_FILETIME ft) { return (static_cast<uint64_t>(ft.hi) << 32) | ft.lo; }

Times process_times() {
  Times t{};
  W32_FILETIME creation{}, exit{}, kernel{}, user{};
  if (!w32.GetProcessTimes(w32.GetCurrentProcess(), &creation, &exit, &kernel, &user)) return t;
  t.user_ticks = filetime_100ns(user) / (10000000 / kClockTicks);
  t.system_ticks = filetime_100ns(kernel) / (10000000 / kClockTicks);
  W32_FILETIME now{};
  w32.GetSystemTimeAsFileTime(&now);
  const uint64_t boot = filetime_100ns(now) - w32.GetTickCount64() * 10000;
  const uint64_t created = filetime_100ns(creation);
  t.start_ticks = created > boot ? (created - boot) / (10000000 / kClockTicks) : 0;
  return t;
}


W32_PROCESS_MEMORY_COUNTERS memory_counters() {
  W32_PROCESS_MEMORY_COUNTERS pmc{};
  pmc.cb = sizeof pmc;
  w32.K32GetProcessMemoryInfo(w32.GetCurrentProcess(), &pmc, sizeof pmc);
  return pmc;
}

uint64_t virtual_size() {
  struct {
    size_t peak_virtual_size, virtual_size;
    unsigned long page_fault_count;
    size_t rest[8];
  } counters{};
  if (w32.NtQueryInformationProcess(w32.GetCurrentProcess(), 3 /* ProcessVmCounters */, &counters, sizeof counters,
                                    nullptr) < 0)
    return memory_counters().pagefile_usage;
  return counters.virtual_size;
}

int current_umask() {
  long old = sys_umask(022, 0, 0, 0, 0, 0);
  sys_umask(old, 0, 0, 0, 0, 0);
  return static_cast<int>(old);
}

struct Code {
  uintptr_t start = 0, end = 0;
  uint64_t lib_bytes = 0;
};

void count_code(void* ctx, uintptr_t start, uintptr_t file_end, uintptr_t, uint64_t, const char* path) {
  auto* c = static_cast<Code*>(ctx);
  W32_MEMORY_BASIC_INFORMATION m;
  if (!w32.VirtualQuery(reinterpret_cast<void*>(start), &m, sizeof m) || (m.protect & 0xF0) == 0) return;
  if (strcmp(path, w32_program) == 0) {
    if (!c->start) c->start = start;
    c->end = file_end;
  } else {
    c->lib_bytes += file_end - start;
  }
}

Code code_ranges() {
  static W32_SRWLOCK lock;
  static Code cached;
  static uint64_t cached_generation = ~0ull;
  const uint64_t generation = w32_loader_generation();
  w32.AcquireSRWLockExclusive(&lock);
  if (cached_generation != generation) {
    Code c;
    w32_loader_segments(count_code, &c);
    cached = c;
    cached_generation = generation;
  }
  Code c = cached;
  w32.ReleaseSRWLockExclusive(&lock);
  return c;
}


void cpu_flags(Text& t) {
  struct Bit {
    unsigned leaf, sub, reg, bit;  // reg: 0 eax, 1 ebx, 2 ecx, 3 edx
    const char* name;
  };
  static const Bit kBits[] = {
      {1, 0, 3, 0, "fpu"}, {1, 0, 3, 1, "vme"}, {1, 0, 3, 2, "de"}, {1, 0, 3, 3, "pse"}, {1, 0, 3, 4, "tsc"},
      {1, 0, 3, 5, "msr"}, {1, 0, 3, 6, "pae"}, {1, 0, 3, 7, "mce"}, {1, 0, 3, 8, "cx8"}, {1, 0, 3, 9, "apic"},
      {1, 0, 3, 11, "sep"}, {1, 0, 3, 12, "mtrr"}, {1, 0, 3, 13, "pge"}, {1, 0, 3, 14, "mca"}, {1, 0, 3, 15, "cmov"},
      {1, 0, 3, 16, "pat"}, {1, 0, 3, 17, "pse36"}, {1, 0, 3, 19, "clflush"}, {1, 0, 3, 21, "dts"},
      {1, 0, 3, 22, "acpi"}, {1, 0, 3, 23, "mmx"}, {1, 0, 3, 24, "fxsr"}, {1, 0, 3, 25, "sse"}, {1, 0, 3, 26, "sse2"},
      {1, 0, 3, 27, "ss"}, {1, 0, 3, 28, "ht"}, {1, 0, 3, 29, "tm"}, {1, 0, 3, 31, "pbe"},
      {0x80000001, 0, 3, 11, "syscall"}, {0x80000001, 0, 3, 20, "nx"}, {0x80000001, 0, 3, 22, "mmxext"},
      {0x80000001, 0, 3, 25, "fxsr_opt"}, {0x80000001, 0, 3, 26, "pdpe1gb"}, {0x80000001, 0, 3, 27, "rdtscp"},
      {0x80000001, 0, 3, 29, "lm"}, {1, 0, 2, 0, "pni"}, {1, 0, 2, 1, "pclmulqdq"}, {1, 0, 2, 2, "dtes64"},
      {1, 0, 2, 3, "monitor"}, {1, 0, 2, 4, "ds_cpl"}, {1, 0, 2, 5, "vmx"}, {1, 0, 2, 6, "smx"}, {1, 0, 2, 7, "est"},
      {1, 0, 2, 8, "tm2"}, {1, 0, 2, 9, "ssse3"}, {1, 0, 2, 12, "fma"}, {1, 0, 2, 13, "cx16"}, {1, 0, 2, 14, "xtpr"},
      {1, 0, 2, 15, "pdcm"}, {1, 0, 2, 17, "pcid"}, {1, 0, 2, 18, "dca"}, {1, 0, 2, 19, "sse4_1"},
      {1, 0, 2, 20, "sse4_2"}, {1, 0, 2, 21, "x2apic"}, {1, 0, 2, 22, "movbe"}, {1, 0, 2, 23, "popcnt"},
      {1, 0, 2, 24, "tsc_deadline_timer"}, {1, 0, 2, 25, "aes"}, {1, 0, 2, 26, "xsave"}, {1, 0, 2, 28, "avx"},
      {1, 0, 2, 29, "f16c"}, {1, 0, 2, 30, "rdrand"}, {1, 0, 2, 31, "hypervisor"},
      {0x80000001, 0, 2, 0, "lahf_lm"}, {0x80000001, 0, 2, 1, "cmp_legacy"}, {0x80000001, 0, 2, 2, "svm"},
      {0x80000001, 0, 2, 5, "abm"}, {0x80000001, 0, 2, 6, "sse4a"}, {0x80000001, 0, 2, 7, "misalignsse"},
      {0x80000001, 0, 2, 8, "3dnowprefetch"}, {0x80000001, 0, 2, 11, "xop"}, {0x80000001, 0, 2, 16, "fma4"},
      {7, 0, 1, 0, "fsgsbase"}, {7, 0, 1, 3, "bmi1"}, {7, 0, 1, 4, "hle"}, {7, 0, 1, 5, "avx2"},
      {7, 0, 1, 7, "smep"}, {7, 0, 1, 8, "bmi2"}, {7, 0, 1, 9, "erms"}, {7, 0, 1, 10, "invpcid"},
      {7, 0, 1, 11, "rtm"}, {7, 0, 1, 16, "avx512f"}, {7, 0, 1, 17, "avx512dq"}, {7, 0, 1, 18, "rdseed"},
      {7, 0, 1, 19, "adx"}, {7, 0, 1, 20, "smap"}, {7, 0, 1, 21, "avx512ifma"}, {7, 0, 1, 23, "clflushopt"},
      {7, 0, 1, 24, "clwb"}, {7, 0, 1, 28, "avx512cd"}, {7, 0, 1, 29, "sha_ni"}, {7, 0, 1, 30, "avx512bw"},
      {7, 0, 1, 31, "avx512vl"}, {7, 0, 2, 1, "avx512vbmi"}, {7, 0, 2, 2, "umip"}, {7, 0, 2, 4, "pku"},
      {7, 0, 2, 6, "avx512_vbmi2"}, {7, 0, 2, 8, "gfni"}, {7, 0, 2, 9, "vaes"}, {7, 0, 2, 10, "vpclmulqdq"},
      {7, 0, 2, 11, "avx512_vnni"}, {7, 0, 2, 12, "avx512_bitalg"}, {7, 0, 2, 14, "avx512_vpopcntdq"},
      {7, 0, 2, 22, "rdpid"}, {7, 0, 3, 4, "fsrm"}, {7, 0, 3, 18, "pconfig"}, {7, 0, 3, 26, "flush_l1d"},
      {7, 0, 3, 29, "arch_capabilities"},
  };
  unsigned max_basic = 0, max_ext = 0, r[4];
  __asm__("cpuid" : "=a"(max_basic), "=b"(r[1]), "=c"(r[2]), "=d"(r[3]) : "a"(0), "c"(0));
  __asm__("cpuid" : "=a"(max_ext), "=b"(r[1]), "=c"(r[2]), "=d"(r[3]) : "a"(0x80000000), "c"(0));
  bool first = true;
  for (const Bit& b : kBits) {
    if ((b.leaf < 0x80000000 && b.leaf > max_basic) || (b.leaf >= 0x80000000 && b.leaf > max_ext)) continue;
    __asm__("cpuid" : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3]) : "a"(b.leaf), "c"(b.sub));
    if (r[b.reg] & (1u << b.bit)) {
      t.add("%s%s", first ? "" : " ", b.name);
      first = false;
    }
  }
}

double cpu_mhz(const char* brand) {
  static double measured;
  if (const char* at = strstr(brand, "GHz")) {
    const char* p = at;
    while (p > brand && (p[-1] == '.' || (p[-1] >= '0' && p[-1] <= '9'))) p--;
    double ghz = strtod(p, nullptr);
    if (ghz > 0) return ghz * 1000;
  }
  if (measured == 0) {
    unsigned lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    const uint64_t t0 = (static_cast<uint64_t>(hi) << 32) | lo;
    const int64_t n0 = w32_monotonic_ns();
    w32.Sleep(20);
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    const uint64_t t1 = (static_cast<uint64_t>(hi) << 32) | lo;
    const int64_t n1 = w32_monotonic_ns();
    measured = n1 > n0 ? static_cast<double>(t1 - t0) * 1000.0 / static_cast<double>(n1 - n0) : 0;
  }
  return measured;
}

void cpuinfo(Text& t) {
  W32_SYSTEM_INFO si;
  w32.GetSystemInfo(&si);
  unsigned r[4];
  char vendor[13] = {};
  __asm__("cpuid" : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3]) : "a"(0), "c"(0));
  memcpy(vendor, &r[1], 4);
  memcpy(vendor + 4, &r[3], 4);
  memcpy(vendor + 8, &r[2], 4);
  const unsigned max_basic = r[0];
  char brand[49] = {};
  for (unsigned i = 0; i < 3; i++) {
    __asm__("cpuid" : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3]) : "a"(0x80000002 + i), "c"(0));
    memcpy(brand + 16 * i, r, 16);
  }
  const char* name = brand;
  while (*name == ' ') name++;
  __asm__("cpuid" : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3]) : "a"(1), "c"(0));
  const unsigned base_family = (r[0] >> 8) & 0xf, base_model = (r[0] >> 4) & 0xf, stepping = r[0] & 0xf;
  const unsigned family = base_family == 0xf ? base_family + ((r[0] >> 20) & 0xff) : base_family;
  const unsigned model = (base_family == 6 || base_family == 0xf) ? base_model + (((r[0] >> 16) & 0xf) << 4) : base_model;
  const unsigned clflush = ((r[1] >> 8) & 0xff) * 8;
  __asm__("cpuid" : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3]) : "a"(0x80000008), "c"(0));
  const unsigned physical_bits = r[0] & 0xff, virtual_bits = (r[0] >> 8) & 0xff;
  const double mhz = cpu_mhz(name);
  const unsigned cpus = si.processor_count ? si.processor_count : 1;
  for (unsigned i = 0; i < cpus; i++) {
    t.add("processor\t: %u\nvendor_id\t: %s\ncpu family\t: %u\nmodel\t\t: %u\nmodel name\t: %s\nstepping\t: %u\n", i,
          vendor, family, model, name, stepping);
    t.add("cpu MHz\t\t: %.3f\nphysical id\t: 0\nsiblings\t: %u\ncore id\t\t: %u\ncpu cores\t: %u\napicid\t\t: %u\n"
          "initial apicid\t: %u\nfpu\t\t: yes\nfpu_exception\t: yes\ncpuid level\t: %u\nwp\t\t: yes\nflags\t\t: ",
          mhz, cpus, i, cpus, i, i, max_basic);
    cpu_flags(t);
    t.add("\nbogomips\t: %.2f\nclflush size\t: %u\ncache_alignment\t: %u\naddress sizes\t: %u bits physical, %u bits "
          "virtual\npower management:\n\n",
          mhz * 2, clflush, clflush, physical_bits, virtual_bits);
  }
}

void meminfo(Text& t) {
  W32_MEMORYSTATUSEX m{};
  m.length = sizeof m;
  w32.GlobalMemoryStatusEx(&m);
  const unsigned long long total = m.total_phys / 1024, avail = m.avail_phys / 1024;
  const unsigned long long swap_total = (m.total_page > m.total_phys ? m.total_page - m.total_phys : 0) / 1024;
  const unsigned long long swap_free = (m.avail_page > m.avail_phys ? m.avail_page - m.avail_phys : 0) / 1024;
  t.add("MemTotal:       %8llu kB\nMemFree:        %8llu kB\nMemAvailable:   %8llu kB\nBuffers:        %8u kB\n"
        "Cached:         %8u kB\nSwapCached:     %8u kB\nSwapTotal:      %8llu kB\nSwapFree:       %8llu kB\n"
        "CommitLimit:    %8llu kB\nCommitted_AS:   %8llu kB\n",
        total, avail, avail, 0u, 0u, 0u, swap_total, swap_free, m.total_page / 1024,
        (m.total_page - m.avail_page) / 1024);
}

void cmdline(Text& t) {
  // zygote rewrites argv to the package name, so cmdline says com.roblox.client like on a phone
  const char* name = w32_process_name();
  t.bytes(name, strlen(name) + 1);
}

void status(Text& t) {
  const W32_PROCESS_MEMORY_COUNTERS pmc = memory_counters();
  const uint64_t vsize = virtual_size();
  const Code code = code_ranges();
  uint64_t blocked, ignored, caught;
  w32_signal_masks(&blocked, &ignored, &caught);
  W32_SYSTEM_INFO si;
  w32.GetSystemInfo(&si);
  const unsigned cpus = si.processor_count ? si.processor_count : 1;
  const unsigned long p = pid();
  const uint64_t stack = w32_boot->stack_high - w32_boot->stack_low;
  t.add("Name:\t%s\nUmask:\t%04o\nState:\tR (running)\nTgid:\t%lu\nNgid:\t0\nPid:\t%lu\nPPid:\t1\nTracerPid:\t0\n"
        "Uid:\t10000\t10000\t10000\t10000\nGid:\t10000\t10000\t10000\t10000\nFDSize:\t256\nGroups:\t\n"
        "NStgid:\t%lu\nNSpid:\t%lu\nNSpgid:\t%lu\nNSsid:\t%lu\n",
        w32_process_comm(), current_umask(), p, p, p, p, p, p);
  const unsigned long long size_kb = vsize / 1024, peak_kb = pmc.peak_pagefile_usage / 1024 > size_kb ? pmc.peak_pagefile_usage / 1024 : size_kb;
  t.add("VmPeak:\t%8llu kB\nVmSize:\t%8llu kB\nVmLck:\t%8u kB\nVmPin:\t%8u kB\nVmHWM:\t%8llu kB\nVmRSS:\t%8llu kB\n"
        "RssAnon:\t%8llu kB\nRssFile:\t%8u kB\nRssShmem:\t%8u kB\nVmData:\t%8llu kB\nVmStk:\t%8llu kB\nVmExe:\t%8llu kB\n"
        "VmLib:\t%8llu kB\nVmPTE:\t%8u kB\nVmSwap:\t%8u kB\nHugetlbPages:\t%8u kB\nCoreDumping:\t0\nTHP_enabled:\t1\n",
        peak_kb, size_kb, 0u, 0u, static_cast<unsigned long long>(pmc.peak_working_set / 1024),
        static_cast<unsigned long long>(pmc.working_set / 1024), static_cast<unsigned long long>(pmc.working_set / 1024),
        0u, 0u, static_cast<unsigned long long>(pmc.pagefile_usage / 1024), static_cast<unsigned long long>(stack / 1024),
        static_cast<unsigned long long>((code.end - code.start) / 1024), static_cast<unsigned long long>(code.lib_bytes / 1024),
        0u, 0u, 0u);
  t.add("Threads:\t%u\nSigQ:\t0/63432\nSigPnd:\t0000000000000000\nShdPnd:\t0000000000000000\nSigBlk:\t%016llx\n"
        "SigIgn:\t%016llx\nSigCgt:\t%016llx\nCapInh:\t0000000000000000\nCapPrm:\t0000000000000000\n"
        "CapEff:\t0000000000000000\nCapBnd:\t000001ffffffffff\nCapAmb:\t0000000000000000\nNoNewPrivs:\t0\nSeccomp:\t0\n"
        "Seccomp_filters:\t0\nSpeculation_Store_Bypass:\tthread vulnerable\nSpeculationIndirectBranch:\tconditional enabled\n",
        thread_count(), static_cast<unsigned long long>(blocked), static_cast<unsigned long long>(ignored),
        static_cast<unsigned long long>(caught));
  t.add("Cpus_allowed:\t%x\nCpus_allowed_list:\t", cpus >= 32 ? 0xffffffffu : (1u << cpus) - 1);
  if (cpus > 1) t.add("0-%u\n", cpus - 1); else t.add("0\n");
  t.add("Mems_allowed:\t00000000,00000001\nMems_allowed_list:\t0\nvoluntary_ctxt_switches:\t0\n"
        "nonvoluntary_ctxt_switches:\t0\n");
}

void statm(Text& t) {
  // in pages: size resident shared text lib data dt
  const W32_PROCESS_MEMORY_COUNTERS pmc = memory_counters();
  const Code code = code_ranges();
  t.add("%llu %llu 0 %llu 0 %llu 0\n", static_cast<unsigned long long>(virtual_size() / 4096),
        static_cast<unsigned long long>(pmc.working_set / 4096), static_cast<unsigned long long>((code.end - code.start) / 4096),
        static_cast<unsigned long long>(pmc.pagefile_usage / 4096));
}

void proc_stat(Text& t) {
  const W32_PROCESS_MEMORY_COUNTERS pmc = memory_counters();
  const Times times = process_times();
  const Code code = code_ranges();
  uint64_t blocked, ignored, caught;
  w32_signal_masks(&blocked, &ignored, &caught);
  const unsigned long p = pid();
  // fields as in man 5 proc, in order
  t.add("%lu (%s) R 1 %lu %lu 0 -1 4194304 %lu 0 0 0 %llu %llu 0 0 20 0 %u 0 %llu %llu %llu 18446744073709551615 "
        "%llu %llu %llu 0 0 0 %llu %llu %llu 0 0 0 17 %lu 0 0 0 0 0 0 0 0 0 0 0 0 0\n",
        p, w32_process_comm(), p, p, static_cast<unsigned long>(pmc.page_fault_count), static_cast<unsigned long long>(times.user_ticks),
        static_cast<unsigned long long>(times.system_ticks), thread_count(), static_cast<unsigned long long>(times.start_ticks),
        static_cast<unsigned long long>(virtual_size()), static_cast<unsigned long long>(pmc.working_set / 4096),
        static_cast<unsigned long long>(code.start), static_cast<unsigned long long>(code.end),
        static_cast<unsigned long long>(w32_boot->stack_high), static_cast<unsigned long long>(blocked),
        static_cast<unsigned long long>(ignored), static_cast<unsigned long long>(caught),
        static_cast<unsigned long>(w32.GetCurrentProcessorNumber()));
}

void auxv(Text& t) {
  for (const uintptr_t* a = w32_auxv; a; a += 2) {
    t.bytes(a, 2 * sizeof(uintptr_t));
    if (a[0] == AT_NULL) break;
  }
}

void root_device(unsigned* major_out, unsigned* minor_out) {
  struct stat st;
  if (sys_newfstatat(AT_FDCWD, reinterpret_cast<long>("/"), reinterpret_cast<long>(&st), 0, 0, 0) < 0) st.st_dev = 0;
  *major_out = major(st.st_dev);
  *minor_out = minor(st.st_dev);
}

void mounts(Text& t) {
  t.add("/dev/root / ext4 rw,relatime 0 0\nproc /proc proc rw,nosuid,nodev,noexec,relatime 0 0\n"
        "sysfs /sys sysfs rw,nosuid,nodev,noexec,relatime 0 0\ndevtmpfs /dev devtmpfs rw,nosuid,relatime,mode=755 0 0\n"
        "devpts /dev/pts devpts rw,nosuid,noexec,relatime,mode=600,ptmxmode=000 0 0\n");
}

void mountinfo(Text& t) {
  unsigned root_major, root_minor;
  root_device(&root_major, &root_minor);
  t.add("21 1 %u:%u / / rw,relatime shared:1 - ext4 /dev/root rw\n", root_major, root_minor);
  t.add("22 21 0:21 / /proc rw,nosuid,nodev,noexec,relatime shared:2 - proc proc rw\n"
        "23 21 0:20 / /sys rw,nosuid,nodev,noexec,relatime shared:3 - sysfs sysfs rw\n"
        "24 21 0:5 / /dev rw,nosuid,relatime shared:4 - devtmpfs devtmpfs rw,mode=755\n"
        "25 24 0:22 / /dev/pts rw,nosuid,noexec,relatime shared:5 - devpts devpts rw,mode=600,ptmxmode=000\n");
}

void limits(Text& t) {
  struct Row {
    int resource;
    const char* name;
    const char* units;
  };
  static const Row kRows[] = {
      {RLIMIT_CPU, "Max cpu time", "seconds"},          {RLIMIT_FSIZE, "Max file size", "bytes"},
      {RLIMIT_DATA, "Max data size", "bytes"},          {RLIMIT_STACK, "Max stack size", "bytes"},
      {RLIMIT_CORE, "Max core file size", "bytes"},     {RLIMIT_RSS, "Max resident set", "bytes"},
      {RLIMIT_NPROC, "Max processes", "processes"},     {RLIMIT_NOFILE, "Max open files", "files"},
      {RLIMIT_MEMLOCK, "Max locked memory", "bytes"},   {RLIMIT_AS, "Max address space", "bytes"},
      {RLIMIT_LOCKS, "Max file locks", "locks"},        {RLIMIT_SIGPENDING, "Max pending signals", "signals"},
      {RLIMIT_MSGQUEUE, "Max msgqueue size", "bytes"},  {RLIMIT_NICE, "Max nice priority", ""},
      {RLIMIT_RTPRIO, "Max realtime priority", ""},     {RLIMIT_RTTIME, "Max realtime timeout", "us"},
  };
  t.add("%-25s %-20s %-20s %-10s\n", "Limit", "Soft Limit", "Hard Limit", "Units");
  for (const Row& row : kRows) {
    struct rlimit rl = {RLIM_INFINITY, RLIM_INFINITY};
    sys_prlimit64(0, row.resource, 0, reinterpret_cast<long>(&rl), 0, 0);
    char soft[24], hard[24];
    if (rl.rlim_cur == RLIM_INFINITY) strcpy(soft, "unlimited"); else snprintf(soft, sizeof soft, "%llu", static_cast<unsigned long long>(rl.rlim_cur));
    if (rl.rlim_max == RLIM_INFINITY) strcpy(hard, "unlimited"); else snprintf(hard, sizeof hard, "%llu", static_cast<unsigned long long>(rl.rlim_max));
    t.add("%-25s %-20s %-20s %-10s\n", row.name, soft, hard, row.units);
  }
}


struct FileRange {
  uintptr_t start, end;
  uint64_t offset;
  const char* path;
};

struct Ranges {
  FileRange* items = nullptr;
  size_t count = 0, cap = 0;
  void add(uintptr_t start, uintptr_t end, uint64_t offset, const char* path) {
    if (start >= end) return;
    if (count == cap) {
      size_t want = cap ? cap * 2 : 64;
      auto* p = static_cast<FileRange*>(realloc(items, want * sizeof(FileRange)));
      if (!p) return;
      items = p;
      cap = want;
    }
    items[count++] = FileRange{start, end, offset, path};
  }
};

void add_segment(void* ctx, uintptr_t start, uintptr_t file_end, uintptr_t, uint64_t offset, const char* path) {
  static_cast<Ranges*>(ctx)->add(start, file_end, offset, path);
}

struct Identity {
  char path[512];
  unsigned dev_major, dev_minor;
  uint64_t inode;
};

struct Identities {
  Identity items[128];
  size_t count = 0;
  const Identity* of(const char* path) {
    for (size_t i = 0; i < count; i++)
      if (strcmp(items[i].path, path) == 0) return &items[i];
    struct stat st;
    if (sys_newfstatat(AT_FDCWD, reinterpret_cast<long>(path), reinterpret_cast<long>(&st), 0, 0, 0) < 0) {
      st.st_dev = 0;
      st.st_ino = 0;
    }
    Identity* id = count < 128 ? &items[count++] : &items[127];
    snprintf(id->path, sizeof id->path, "%s", path);
    id->dev_major = major(st.st_dev);
    id->dev_minor = minor(st.st_dev);
    id->inode = st.st_ino;
    return id;
  }
};

struct Drive {
  WCHAR device[128];
  size_t length;
  WCHAR letter;
};
Drive g_drives[26];
int g_drive_count = -1;

void load_drives() {
  if (g_drive_count >= 0) return;
  int n = 0;
  for (WCHAR letter = 'A'; letter <= 'Z'; letter++) {
    WCHAR name[3] = {letter, ':', 0};
    WCHAR target[128];
    if (!w32.QueryDosDeviceW(name, target, 128)) continue;
    size_t len = 0;
    while (target[len]) len++;
    memcpy(g_drives[n].device, target, (len + 1) * sizeof(WCHAR));
    g_drives[n].length = len;
    g_drives[n].letter = letter;
    n++;
  }
  g_drive_count = n;
}

bool mapped_file(uintptr_t a, char* out, size_t cap) {
  WCHAR nt[W32_PATH_MAX];
  DWORD n = w32.K32GetMappedFileNameW(w32.GetCurrentProcess(), reinterpret_cast<void*>(a), nt, W32_PATH_MAX);
  if (n == 0 || n >= W32_PATH_MAX) return false;
  load_drives();
  for (int i = 0; i < g_drive_count; i++) {
    const Drive& d = g_drives[i];
    if (n > d.length && nt[d.length] == '\\' && memcmp(nt, d.device, d.length * sizeof(WCHAR)) == 0) {
      WCHAR dos[W32_PATH_MAX];
      dos[0] = d.letter;
      dos[1] = ':';
      memcpy(dos + 2, nt + d.length, (n - d.length + 1) * sizeof(WCHAR));
      return w32_unix_path(dos, out, cap) == 0;
    }
  }
  return false;
}

void map_line(Text& t, uintptr_t start, uintptr_t end, const char* perms, uint64_t offset, unsigned dev_major,
              unsigned dev_minor, uint64_t inode, const char* name) {
  char line[160];
  int n = snprintf(line, sizeof line, "%08lx-%08lx %s %08llx %02x:%02x %llu ", static_cast<unsigned long>(start),
                   static_cast<unsigned long>(end), perms, static_cast<unsigned long long>(offset), dev_major,
                   dev_minor, static_cast<unsigned long long>(inode));
  if (n < 0) return;
  t.bytes(line, static_cast<size_t>(n));
  if (name) {
    // linux pads the name to column 73 (task_mmu.c)
    static const char kSpaces[] = "                                                                        ";
    if (n < 72) t.bytes(kSpaces, static_cast<size_t>(72 - n));
    t.bytes(" ", 1);
    t.bytes(name, strlen(name));
  }
  t.bytes("\n", 1);
}

void permissions(DWORD protect, DWORD state, bool shared, char out[5]) {
  DWORD p = protect & 0xff;
  bool r = false, w = false, x = false;
  if (state == MEM_COMMIT && !(protect & 0x100 /* PAGE_GUARD */)) {
    x = p == 0x10 || p == 0x20 || p == 0x40 || p == 0x80;
    w = p == 0x04 || p == 0x08 || p == 0x40 || p == 0x80;
    r = p != 0x01 && p != 0x10 && p != 0;
  }
  out[0] = r ? 'r' : '-';
  out[1] = w ? 'w' : '-';
  out[2] = x ? 'x' : '-';
  out[3] = shared ? 's' : 'p';
  out[4] = 0;
}

void anon_lines(Text& t, uintptr_t start, uintptr_t end, const char* perms, const W32AnonName* names, size_t count,
                size_t* cursor) {
  while (*cursor < count && names[*cursor].end <= start) (*cursor)++;
  uintptr_t at = start;
  char label[96];
  for (size_t i = *cursor; at < end; i++) {
    const bool more = i < count && names[i].start < end;
    const uintptr_t next = more ? (names[i].start > at ? names[i].start : at) : end;
    if (next > at) {
      map_line(t, at, next, perms, 0, 0, 0, 0, nullptr);
      at = next;
    }
    if (!more) break;
    if (names[i].end <= at) continue;
    const uintptr_t piece_end = names[i].end < end ? names[i].end : end;
    snprintf(label, sizeof label, "[anon:%s]", names[i].name);
    map_line(t, at, piece_end, perms, 0, 0, 0, 0, label);
    at = piece_end;
  }
}

void maps(Text& t) {
  Ranges files;
  w32_loader_segments(add_segment, &files);
  size_t view_count = w32_mem_file_views(nullptr, 0);
  W32FileView* views = nullptr;
  if (view_count) {
    view_count += 16;
    views = static_cast<W32FileView*>(malloc(view_count * sizeof(W32FileView)));
    size_t n = views ? w32_mem_file_views(views, view_count) : 0;
    if (n < view_count) view_count = n;
    for (size_t i = 0; views && i < view_count; i++) files.add(views[i].start, views[i].end, views[i].offset, views[i].path);
  }
  for (size_t i = 1; i < files.count; i++) {
    FileRange r = files.items[i];
    size_t j = i;
    while (j > 0 && files.items[j - 1].start > r.start) {
      files.items[j] = files.items[j - 1];
      j--;
    }
    files.items[j] = r;
  }
  size_t name_count = w32_mem_anon_names(nullptr, 0);
  W32AnonName* names = nullptr;
  if (name_count) {
    name_count += 16;
    names = static_cast<W32AnonName*>(malloc(name_count * sizeof(W32AnonName)));
    size_t n = names ? w32_mem_anon_names(names, name_count) : 0;
    if (n < name_count) name_count = n;
    for (size_t i = 1; i < name_count; i++) {
      W32AnonName r = names[i];
      size_t j = i;
      while (j > 0 && names[j - 1].start > r.start) {
        names[j] = names[j - 1];
        j--;
      }
      names[j] = r;
    }
  }
  size_t name_cursor = 0;
  auto* ids = static_cast<Identities*>(calloc(1, sizeof(Identities)));
  const uintptr_t stack_low = w32_boot->stack_low, stack_high = w32_boot->stack_high;
  char image[W32_PATH_MAX];
  void* image_base = nullptr;
  bool image_named = false;
  uintptr_t a = 0x10000;
  W32_MEMORY_BASIC_INFORMATION m;
  size_t cursor = 0;
  // our own runs skip VirtualQuery, asking about every region cost 70 ms a read
  uintptr_t run_end = 0;
  DWORD run_protect = 0, run_state = 0;
  unsigned queries = 0, reused = 0;
  int64_t query_ns = 0, slowest_ns = 0;
  W32_MEMORY_BASIC_INFORMATION slowest{};
  int64_t committed_queried = 0;
  for (;;) {
    if (a >= kUserEnd) break;
    if (w32_mem_run_at(a, &run_end, &run_protect, &run_state)) {
      m = W32_MEMORY_BASIC_INFORMATION{reinterpret_cast<void*>(a), nullptr, 0, 0, run_end - a, run_state, run_protect,
                                       MEM_PRIVATE, 0};
      reused++;
    } else {
      const int64_t q0 = w32_monotonic_ns();
      if (!w32.VirtualQuery(reinterpret_cast<void*>(a), &m, sizeof m)) break;
      const int64_t took = w32_monotonic_ns() - q0;
      queries++;
      query_ns += took;
      if (m.state == MEM_COMMIT) committed_queried += static_cast<int64_t>(m.region_size);
      if (took > slowest_ns) {
        slowest_ns = took;
        slowest = m;
      }
    }
    const uintptr_t start = reinterpret_cast<uintptr_t>(m.base), end = start + m.region_size;
    if (end <= a) break;
    a = end;
    if (m.state == MEM_FREE) continue;
    char perms[5];
    permissions(m.protect, m.state, m.type == MEM_MAPPED, perms);
    if (m.type == MEM_IMAGE || m.type == MEM_MAPPED) {
      if (m.allocation_base != image_base) {
        image_base = m.allocation_base;
        image_named = mapped_file(start, image, sizeof image);
      }
      const Identity* id = image_named && ids ? ids->of(image) : nullptr;
      map_line(t, start, end, perms, start - reinterpret_cast<uintptr_t>(m.allocation_base), id ? id->dev_major : 0,
               id ? id->dev_minor : 0, id ? id->inode : 0, image_named ? image : nullptr);
      continue;
    }
    while (cursor < files.count && files.items[cursor].end <= start) cursor++;
    uintptr_t at = start;
    for (size_t i = cursor; at < end; i++) {
      const bool more = i < files.count && files.items[i].start < end;
      const uintptr_t next = more ? (files.items[i].start > at ? files.items[i].start : at) : end;
      if (next > at) {
        const bool stack = at >= stack_low && next <= stack_high && m.state == MEM_COMMIT;
        if (stack) map_line(t, at, next, perms, 0, 0, 0, 0, "[stack]");
        else anon_lines(t, at, next, perms, names, name_count, &name_cursor);
        at = next;
      }
      if (!more) break;
      const FileRange& f = files.items[i];
      if (f.end <= at) continue;
      const uintptr_t piece_end = f.end < end ? f.end : end;
      const Identity* id = ids ? ids->of(f.path) : nullptr;
      map_line(t, at, piece_end, perms, f.offset + (at - f.start), id ? id->dev_major : 0, id ? id->dev_minor : 0,
               id ? id->inode : 0, f.path);
      at = piece_end;
    }
  }
  free(ids);
  free(names);
  free(views);
  free(files.items);
  static int detail_left = 40;
  if (query_ns >= 10000000 && __atomic_fetch_sub(&detail_left, 1, __ATOMIC_RELAXED) > 0) {
    w32_debug("procfs: maps asked VirtualQuery %u times (%.1f ms, %.0f MB committed among them), reused %u runs; "
              "slowest %.1f ms at %p: %.1f MB, state 0x%lx type 0x%lx protect 0x%lx",
              queries, static_cast<double>(query_ns) / 1e6, static_cast<double>(committed_queried) / 1048576.0, reused,
              static_cast<double>(slowest_ns) / 1e6, slowest.base, static_cast<double>(slowest.region_size) / 1048576.0,
              static_cast<unsigned long>(slowest.state), static_cast<unsigned long>(slowest.type),
              static_cast<unsigned long>(slowest.protect));
  }
}


struct Entry {
  const char* path;
  W32ProcNode node;
  void (*fill)(Text&);
};

void sys_cpu_range(Text& t) {
  W32_SYSTEM_INFO si;
  w32.GetSystemInfo(&si);
  if (si.processor_count > 1) t.add("0-%u\n", si.processor_count - 1); else t.add("0\n");
}
void comm(Text& t) { t.add("%s\n", w32_process_comm()); }
void cgroup(Text& t) { t.add("0::/\n"); }
void zero(Text& t) { t.add("0\n"); }
void net_unix(Text& t) { t.add("Num       RefCount Protocol Flags    Type St Inode Path\n"); }

const Entry kEntries[] = {
    {"/proc", W32ProcNode::kDirectory, nullptr},
    {"/proc/cpuinfo", W32ProcNode::kFile, cpuinfo},
    {"/proc/meminfo", W32ProcNode::kFile, meminfo},
    {"/proc/self", W32ProcNode::kDirectory, nullptr},
    {"/proc/self/auxv", W32ProcNode::kFile, auxv},
    {"/proc/self/cgroup", W32ProcNode::kFile, cgroup},
    {"/proc/self/cmdline", W32ProcNode::kFile, cmdline},
    {"/proc/self/comm", W32ProcNode::kFile, comm},
    {"/proc/self/cwd", W32ProcNode::kLink, nullptr},
    {"/proc/self/exe", W32ProcNode::kLink, nullptr},
    {"/proc/self/fd", W32ProcNode::kDirectory, nullptr},
    {"/proc/self/limits", W32ProcNode::kFile, limits},
    {"/proc/self/maps", W32ProcNode::kFile, maps},
    {"/proc/self/mountinfo", W32ProcNode::kFile, mountinfo},
    {"/proc/self/mounts", W32ProcNode::kFile, mounts},
    {"/proc/self/net", W32ProcNode::kDirectory, nullptr},
    {"/proc/self/net/unix", W32ProcNode::kFile, net_unix},
    {"/proc/self/oom_adj", W32ProcNode::kFile, zero},
    {"/proc/self/oom_score", W32ProcNode::kFile, zero},
    {"/proc/self/oom_score_adj", W32ProcNode::kFile, zero},
    {"/proc/self/root", W32ProcNode::kLink, nullptr},
    {"/proc/self/stat", W32ProcNode::kFile, proc_stat},
    {"/proc/self/statm", W32ProcNode::kFile, statm},
    {"/proc/self/status", W32ProcNode::kFile, status},
    {"/proc/sys", W32ProcNode::kDirectory, nullptr},
    {"/proc/sys/vm", W32ProcNode::kDirectory, nullptr},
    {"/proc/sys/vm/overcommit_memory", W32ProcNode::kFile, zero},
    {"/sys", W32ProcNode::kDirectory, nullptr},
    {"/sys/devices", W32ProcNode::kDirectory, nullptr},
    {"/sys/devices/system", W32ProcNode::kDirectory, nullptr},
    {"/sys/devices/system/cpu", W32ProcNode::kDirectory, nullptr},
    {"/sys/devices/system/cpu/online", W32ProcNode::kFile, sys_cpu_range},
    {"/sys/devices/system/cpu/possible", W32ProcNode::kFile, sys_cpu_range},
    {"/sys/devices/system/cpu/present", W32ProcNode::kFile, sys_cpu_range},
};

const Entry* find(const char* abs) {
  for (const Entry& e : kEntries)
    if (is(abs, e.path)) return &e;
  return nullptr;
}

bool fd_link(const char* abs, int* fd) {
  if (strncmp(abs, "/proc/self/fd/", 14) != 0 || !abs[14]) return false;
  char* end;
  long n = strtol(abs + 14, &end, 10);
  if (*end || n < 0 || n > 0x7fffffff) return false;
  W32File* f = w32_fd_get(static_cast<int>(n));
  if (!f) return false;
  w32_file_put(f);
  *fd = static_cast<int>(n);
  return true;
}

const char* child_name(const char* parent, const char* path) {
  size_t n = strlen(parent);
  if (strncmp(path, parent, n) != 0 || path[n] != '/') return nullptr;
  const char* name = path + n + 1;
  return strchr(name, '/') ? nullptr : name;
}

struct FdList {
  void (*add)(void*, const char*, unsigned char);
  void* ctx;
};

}  // namespace

void w32_proc_alias(char* abs) {
  if (strncmp(abs, "/proc/", 6) != 0) return;
  const char* p = abs + 6;
  const char* rest = nullptr;
  const char* to = "/proc/self";
  if (strncmp(p, "thread-self", 11) == 0 && (p[11] == 0 || p[11] == '/')) {
    rest = p + 11;
  } else if (strcmp(p, "mounts") == 0) {
    rest = "";
    to = "/proc/self/mounts";
  } else if (strncmp(p, "net", 3) == 0 && (p[3] == 0 || p[3] == '/')) {
    rest = p + 3;
    to = "/proc/self/net";
  } else if (p[0] >= '1' && p[0] <= '9') {
    char* end;
    unsigned long n = strtoul(p, &end, 10);
    if (n == pid() && (*end == 0 || *end == '/')) rest = end;
  }
  if (!rest) return;
  char tail[W32_PATH_MAX];
  snprintf(tail, sizeof tail, "%s", rest);
  snprintf(abs, W32_PATH_MAX, "%s%s", to, tail);
}

W32ProcNode w32_proc_node(const char* abs) {
  if (const Entry* e = find(abs)) return e->node;
  int fd;
  if (fd_link(abs, &fd)) return W32ProcNode::kLink;
  return W32ProcNode::kNone;
}

bool w32_proc_read(const char* abs, char** data, size_t* size) {
  const Entry* e = find(abs);
  if (!e || e->node != W32ProcNode::kFile) return false;
  Text t;
  const int64_t began = w32_monotonic_ns();
  e->fill(t);
  static int slow_left = 300;
  const double ms = static_cast<double>(w32_monotonic_ns() - began) / 1e6;
  if (ms >= 2.0 && __atomic_fetch_sub(&slow_left, 1, __ATOMIC_RELAXED) > 0) {
    w32_debug("procfs: %s took %.1f ms (%zu bytes), thread %lu '%s'", abs, ms, t.size,
              static_cast<unsigned long>(w32.GetCurrentThreadId()), w32_thread_comm());
  }
  if (!t.data) t.grow(0);
  *data = t.data;
  *size = t.size;
  return t.data != nullptr;
}

bool w32_proc_readlink(const char* abs, char* out, size_t cap) {
  int fd;
  if (fd_link(abs, &fd)) {
    W32File* f = w32_fd_get(fd);
    if (!f) return false;
    if (f->upath) snprintf(out, cap, "%s", f->upath);
    else snprintf(out, cap, f->kind == W32Kind::kSocket ? "socket:[%d]" : "pipe:[%d]", fd);
    w32_file_put(f);
    return true;
  }
  if (is(abs, "/proc/self/exe")) {
    if (w32_program[0] != '\0') {
      snprintf(out, cap, "%s", w32_program);
    } else {
      WCHAR exe[W32_PATH_MAX];
      w32.GetModuleFileNameW(nullptr, exe, W32_PATH_MAX);
      if (w32_unix_path(exe, out, cap) < 0) return false;
    }
    return true;
  }
  if (is(abs, "/proc/self/cwd")) return sys_getcwd(reinterpret_cast<long>(out), static_cast<long>(cap), 0, 0, 0, 0) > 0;
  if (is(abs, "/proc/self/root")) {
    snprintf(out, cap, "/");
    return true;
  }
  if (is(abs, "/proc/self")) {
    snprintf(out, cap, "%lu", pid());
    return true;
  }
  return false;
}

bool w32_proc_list(const char* abs, void (*add)(void*, const char*, unsigned char), void* ctx) {
  const Entry* dir = find(abs);
  if (!dir || dir->node != W32ProcNode::kDirectory) return false;
  add(ctx, ".", DT_DIR);
  add(ctx, "..", DT_DIR);
  if (is(abs, "/proc")) {
    char name[24];
    snprintf(name, sizeof name, "%lu", pid());
    add(ctx, name, DT_DIR);
    add(ctx, "self", DT_LNK);
    add(ctx, "thread-self", DT_LNK);
    add(ctx, "mounts", DT_LNK);
    add(ctx, "net", DT_LNK);
  }
  if (is(abs, "/proc/self/fd")) {
    FdList list{add, ctx};
    w32_each_fd(
        [](void* c, int fd) {
          auto* l = static_cast<FdList*>(c);
          char name[16];
          snprintf(name, sizeof name, "%d", fd);
          l->add(l->ctx, name, DT_LNK);
        },
        &list);
    return true;
  }
  for (const Entry& e : kEntries) {
    const char* name = child_name(abs, e.path);
    if (!name || (is(abs, "/proc") && is(name, "self"))) continue;
    add(ctx, name, e.node == W32ProcNode::kDirectory ? DT_DIR : e.node == W32ProcNode::kLink ? DT_LNK : DT_REG);
  }
  return true;
}

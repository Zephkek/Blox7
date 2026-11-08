// built against bionic's headers, so no windows sdk here: the few win32 bits we need are declared below
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <sys/cdefs.h>

#include "../include/w32boot.h"

#define WINAPI __attribute__((ms_abi))

typedef void* HANDLE;
typedef int BOOL;
typedef uint32_t DWORD;
typedef uint16_t WCHAR;
typedef uint64_t SOCKET;
#define INVALID_HANDLE_VALUE (reinterpret_cast<HANDLE>(static_cast<intptr_t>(-1)))
#define INVALID_SOCKET (~static_cast<SOCKET>(0))

struct W32_FILETIME { DWORD lo, hi; };
struct W32_BY_HANDLE_FILE_INFORMATION {
  DWORD attributes;
  W32_FILETIME creation, access, write;
  DWORD volume_serial, size_hi, size_lo, links, index_hi, index_lo;
};
struct W32_WIN32_FIND_DATAW {
  DWORD attributes;
  W32_FILETIME creation, access, write;
  DWORD size_hi, size_lo, reserved0, reserved1;
  WCHAR name[260];
  WCHAR alternate[14];
};
struct W32_SYSTEM_INFO {
  uint16_t arch, reserved;
  DWORD page_size;
  void *min_address, *max_address;
  uintptr_t active_processors;
  DWORD processor_count, processor_type, allocation_granularity;
  uint16_t level, revision;
};
struct W32_MEMORY_BASIC_INFORMATION {
  void* base;
  void* allocation_base;
  DWORD allocation_protect;
  DWORD pad0;
  size_t region_size;
  DWORD state, protect, type, pad1;
};
struct W32_MEMORYSTATUSEX {
  DWORD length, load;
  uint64_t total_phys, avail_phys, total_page, avail_page, total_virtual, avail_virtual, avail_ext;
};
struct W32_PROCESS_MEMORY_COUNTERS {
  DWORD cb, page_fault_count;
  size_t peak_working_set, working_set, quota_peak_paged_pool, quota_paged_pool, quota_peak_nonpaged_pool,
      quota_nonpaged_pool, pagefile_usage, peak_pagefile_usage;
};
struct W32_OVERLAPPED {
  uintptr_t internal, internal_high;
  DWORD offset, offset_high;
  HANDLE event;
};
struct W32_SRWLOCK { void* ptr; };
struct W32_CONDITION_VARIABLE { void* ptr; };
struct W32_WSABUF { uint32_t len; char* buf; };
struct W32_WSAPOLLFD { SOCKET fd; int16_t events, revents; };
struct W32_ADDRINFOW {
  int flags, family, socktype, protocol;
  size_t addrlen;
  WCHAR* canonname;
  void* addr;
  W32_ADDRINFOW* next;
};

enum : DWORD {
  W32_ERROR_SUCCESS = 0, W32_ERROR_FILE_NOT_FOUND = 2, W32_ERROR_PATH_NOT_FOUND = 3,
  W32_ERROR_ACCESS_DENIED = 5, W32_ERROR_INVALID_HANDLE = 6, W32_ERROR_NOT_ENOUGH_MEMORY = 8,
  W32_ERROR_NO_MORE_FILES = 18, W32_ERROR_SHARING_VIOLATION = 32, W32_ERROR_HANDLE_EOF = 38,
  W32_ERROR_FILE_EXISTS = 80, W32_ERROR_INVALID_PARAMETER = 87, W32_ERROR_BROKEN_PIPE = 109,
  W32_ERROR_DISK_FULL = 112, W32_ERROR_INVALID_NAME = 123, W32_ERROR_DIR_NOT_EMPTY = 145,
  W32_ERROR_ALREADY_EXISTS = 183, W32_ERROR_NO_DATA = 232, W32_ERROR_DIRECTORY = 267,
  W32_ERROR_IO_PENDING = 997, W32_ERROR_TIMEOUT = 1460, W32_WAIT_TIMEOUT = 258,
};

struct W32Api {
#define W32_FN(ret, name, ...) ret(WINAPI* name)(__VA_ARGS__);
#include "w32_api.inc"
#undef W32_FN
};
extern W32Api w32;

extern struct w32boot* w32_boot;
extern char w32_program[];
extern int w32_tls_index;

void w32_init_api();
void w32_init_threads();
void w32_thread_attach_fs();
extern bool g_w32_fs_modern;
void w32_fs_rearm_now();
inline void w32_fs_rearm() {
  if (__builtin_expect(g_w32_fs_modern, 0)) w32_fs_rearm_now();
}
struct WinContext;
bool w32_fs_redirect(WinContext* c);

[[noreturn]] void w32_fatal(const char* message);
void w32_debug(const char* format, ...) __printflike(1, 2);
bool w32_log_wanted();

int w32_utf8_to_utf16(const char* in, WCHAR* out, size_t out_units);
int w32_utf16_to_utf8(const WCHAR* in, char* out, size_t out_bytes);

long w32_unix_path(const WCHAR* win, char* out, size_t cap);

enum class W32ProcNode { kNone, kFile, kDirectory, kLink };
W32ProcNode w32_proc_node(const char* abs);
bool w32_proc_read(const char* abs, char** data, size_t* size);
bool w32_proc_readlink(const char* abs, char* out, size_t cap);
bool w32_proc_list(const char* abs, void (*add)(void* ctx, const char* name, unsigned char type), void* ctx);
void w32_proc_alias(char* abs);
void w32_each_fd(void (*fn)(void* ctx, int fd), void* ctx);

void w32_loader_segments(void (*fn)(void* ctx, uintptr_t start, uintptr_t file_end, uintptr_t end, uint64_t offset,
                                    const char* path),
                         void* ctx);
uint64_t w32_loader_generation();

extern bool g_w32_stats;
void w32_stats_start();
uintptr_t w32_stats_caller();
void w32_stats_sleep(int64_t asked_ns, int64_t slept_ns, int64_t spun_ns, uint32_t yields, uintptr_t caller);
void w32_stats_futex_wait(int64_t waited_ns, int64_t late_ns, long result, uintptr_t caller);
void w32_stats_futex_wake(long woken);
void w32_stats_yield(uintptr_t caller);
void w32_stats_madvise(int advice, size_t bytes);
void w32_stall_start();

extern "C" void w32_init_vex(void);
extern "C" void w32_vex_set_roblox_range(uintptr_t text_lo, uintptr_t text_hi);
extern "C" void w32_vex_arm_thread(void);
extern "C" void w32_vex_arm_thread_suspended(HANDLE thread);
int w32_thread_count();
char* w32_thread_comm();
void w32_mem_forget_runs(uintptr_t start, uintptr_t end);
void w32_mem_note_run(uintptr_t start, uintptr_t end, uintptr_t allocation, DWORD protect);
bool w32_mem_run_at(uintptr_t a, uintptr_t* end, DWORD* protect, DWORD* state);
DWORD w32_present_thread();
DWORD w32_main_thread_id();
void w32_note_thread_name(const char* comm);
const char* w32_thread_name_of(DWORD id);
const char* w32_process_comm();
const char* w32_process_name();
struct W32FileView {
  uintptr_t start, end;
  uint64_t offset;
  char path[512];
};
size_t w32_mem_file_views(W32FileView* out, size_t cap);
struct W32AnonName {
  uintptr_t start, end;
  char name[80];
};
size_t w32_mem_anon_names(W32AnonName* out, size_t cap);
long w32_mem_set_anon_name(uintptr_t start, size_t length, const char* name);
void w32_signal_masks(uint64_t* blocked, uint64_t* ignored, uint64_t* caught);
extern const uintptr_t* w32_auxv;

int w32_errno_from_win32(DWORD error);
int w32_errno_from_wsa(int error);
inline long w32_fail() { return -w32_errno_from_win32(w32.GetLastError()); }

int64_t w32_monotonic_ns();
int64_t w32_realtime_ns();

long w32_futex_wake(const volatile int* addr, int count);

int64_t w32_filetime_to_ns(W32_FILETIME ft);
W32_FILETIME w32_ns_to_filetime(int64_t ns);

// posix paths live under BLOX7_ROOT, /mnt/<drive>/... is the real drive
constexpr size_t W32_PATH_MAX = 4096;
long w32_path(int dirfd, const char* path, WCHAR* out, size_t out_units);
const char* w32_root_utf8();

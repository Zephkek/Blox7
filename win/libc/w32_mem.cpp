// linux maps 4 KB pages anywhere, windows reserves 64 KB at a time and commits 4 KB inside

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "w32_fd.h"
#include "w32_kernel.h"
#include "w32_syscalls.h"

namespace {

constexpr DWORD MEM_COMMIT = 0x1000, MEM_RESERVE = 0x2000, MEM_DECOMMIT = 0x4000, MEM_RELEASE = 0x8000;
constexpr DWORD MEM_FREE = 0x10000, MEM_RESET = 0x80000, MEM_PRIVATE = 0x20000, MEM_MAPPED = 0x40000;
constexpr DWORD PAGE_NOACCESS = 0x01, PAGE_READONLY = 0x02, PAGE_READWRITE = 0x04, PAGE_GUARD = 0x100;
constexpr DWORD FILE_MAP_WRITE = 0x2, FILE_MAP_READ = 0x4;
constexpr DWORD GENERIC_READ = 0x80000000, GENERIC_WRITE = 0x40000000, FILE_SHARE_ALL = 7, OPEN_EXISTING = 3;
constexpr DWORD FILE_FLAG_BACKUP_SEMANTICS = 0x02000000, VOLUME_NAME_NT = 0x2;
constexpr uintptr_t kPage = 4096, kGranularity = 65536;

DWORD protection(int prot) {
  bool r = prot & PROT_READ, w = prot & PROT_WRITE, x = prot & PROT_EXEC;
  if (x) return w ? 0x40 : (r ? 0x20 : 0x10);
  if (w) return 0x04;
  if (r) return 0x02;
  return PAGE_NOACCESS;
}

int prot_of(DWORD protect) {
  switch (protect & 0xff) {
    case 0x02: return PROT_READ;
    case 0x04: case 0x08: return PROT_READ | PROT_WRITE;
    case 0x10: return PROT_EXEC;
    case 0x20: return PROT_READ | PROT_EXEC;
    case 0x40: case 0x80: return PROT_READ | PROT_WRITE | PROT_EXEC;
    default: return PROT_NONE;
  }
}

uintptr_t page_up(uintptr_t v) { return (v + kPage - 1) & ~(kPage - 1); }

template <typename Fn>
bool for_regions(uintptr_t start, uintptr_t end, Fn fn) {
  uintptr_t a = start;
  while (a < end) {
    W32_MEMORY_BASIC_INFORMATION mbi;
    if (!w32.VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof(mbi))) return false;
    uintptr_t rend = reinterpret_cast<uintptr_t>(mbi.base) + mbi.region_size;
    uintptr_t e = rend < end ? rend : end;
    if (!fn(a, e, mbi)) return false;
    a = e;
  }
  return true;
}

uintptr_t allocation_end(void* allocation_base) {
  uintptr_t a = reinterpret_cast<uintptr_t>(allocation_base);
  for (;;) {
    W32_MEMORY_BASIC_INFORMATION mbi;
    if (!w32.VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof(mbi)) || mbi.allocation_base != allocation_base) return a;
    a = reinterpret_cast<uintptr_t>(mbi.base) + mbi.region_size;
  }
}

bool all_free(uintptr_t start, uintptr_t end) {
  return for_regions(start, end, [](uintptr_t, uintptr_t, const W32_MEMORY_BASIC_INFORMATION& m) { return m.state == MEM_FREE; });
}

bool none_free_in(uintptr_t start, uintptr_t end, uintptr_t* allocation) {
  uintptr_t base = 0;
  bool one = true;
  const bool ok = for_regions(start, end, [&](uintptr_t, uintptr_t, const W32_MEMORY_BASIC_INFORMATION& m) {
    const uintptr_t b = reinterpret_cast<uintptr_t>(m.allocation_base);
    if (base == 0) base = b; else if (b != base) one = false;
    return m.state != MEM_FREE;
  });
  *allocation = ok && one ? base : 0;
  return ok;
}

// runs of pages we know are committed. VirtualQuery walks page tables and ate a quarter of roblox's workers
struct Run {
  uintptr_t start, end;
  uintptr_t allocation;
  DWORD protect;
  DWORD state;
};
constexpr int kRuns = 1024;
Run g_runs[kRuns];
int g_run_next;
int g_run_hint;
uint64_t g_run_generation;
W32_SRWLOCK g_runs_lock;

const Run* find_run_locked(uintptr_t start, uintptr_t end) {
  const int hint = __atomic_load_n(&g_run_hint, __ATOMIC_RELAXED);
  if (g_runs[hint].start <= start && end <= g_runs[hint].end) return &g_runs[hint];
  for (int i = 0; i < kRuns; i++) {
    if (g_runs[i].start <= start && end <= g_runs[i].end) {
      __atomic_store_n(&g_run_hint, i, __ATOMIC_RELAXED);
      return &g_runs[i];
    }
  }
  return nullptr;
}

bool cached_run(uintptr_t start, uintptr_t end, DWORD* protect) {
  w32.AcquireSRWLockShared(&g_runs_lock);
  const Run* r = find_run_locked(start, end);
  const bool committed = r && r->state == MEM_COMMIT;
  if (committed) *protect = r->protect;
  w32.ReleaseSRWLockShared(&g_runs_lock);
  return committed;
}

bool known_reservation(uintptr_t start, uintptr_t end, uintptr_t* allocation) {
  w32.AcquireSRWLockShared(&g_runs_lock);
  const Run* r = find_run_locked(start, end);
  if (r) *allocation = r->allocation;
  w32.ReleaseSRWLockShared(&g_runs_lock);
  return r != nullptr;
}

uint64_t run_generation() { return __atomic_load_n(&g_run_generation, __ATOMIC_ACQUIRE); }

void insert_run_locked(Run r) {
  if (r.start >= r.end || r.allocation == 0) return;
  for (bool merged = true; merged;) {
    merged = false;
    for (int i = 0; i < kRuns; i++) {
      Run& o = g_runs[i];
      if (o.end != 0 && o.allocation == r.allocation && o.protect == r.protect && o.state == r.state && o.start <= r.end && r.start <= o.end) {
        if (o.start < r.start) r.start = o.start;
        if (o.end > r.end) r.end = o.end;
        o = Run{0, 0, 0, 0, 0};
        merged = true;
      }
    }
  }
  for (int i = 0; i < kRuns; i++) {
    if (g_runs[i].end == 0) {
      g_runs[i] = r;
      return;
    }
  }
  g_runs[g_run_next] = r;
  g_run_next = (g_run_next + 1) % kRuns;
}

void forget_runs_locked(uintptr_t start, uintptr_t end) {
  __atomic_store_n(&g_run_generation, g_run_generation + 1, __ATOMIC_RELEASE);
  Run parts[16];  // past 16 we just forget the rest
  int kept = 0;
  for (int i = 0; i < kRuns; i++) {
    Run& o = g_runs[i];
    if (o.end == 0 || o.end <= start || o.start >= end) continue;
    if (o.start < start && kept < 16) parts[kept++] = Run{o.start, start, o.allocation, o.protect, o.state};
    if (o.end > end && kept < 16) parts[kept++] = Run{end, o.end, o.allocation, o.protect, o.state};
    o = Run{0, 0, 0, 0, 0};
  }
  for (int i = 0; i < kept; i++) insert_run_locked(parts[i]);
}

void remember_run(uintptr_t start, uintptr_t end, uintptr_t allocation, DWORD protect, uint64_t seen) {
  w32.AcquireSRWLockExclusive(&g_runs_lock);
  if (g_run_generation == seen) insert_run_locked(Run{start, end, allocation, protect, MEM_COMMIT});
  w32.ReleaseSRWLockExclusive(&g_runs_lock);
}

void set_run(uintptr_t start, uintptr_t end, uintptr_t allocation, DWORD protect, DWORD state) {
  w32.AcquireSRWLockExclusive(&g_runs_lock);
  forget_runs_locked(start, end);
  if (allocation != 0 && (state == MEM_RESERVE || protect != 0))
    insert_run_locked(Run{start, end, allocation, state == MEM_RESERVE ? 0 : protect, state});
  w32.ReleaseSRWLockExclusive(&g_runs_lock);
}

struct ForgetRuns {
  uintptr_t start, end;
  ~ForgetRuns() {
    if (end > start) w32_mem_forget_runs(start, end);
  }
};

struct FilePut {
  W32File* f;
  ~FilePut() {
    if (f) w32_file_put(f);
  }
};

bool replace_pages(uintptr_t start, uintptr_t end, int prot) {
  size_t len = end - start;
  w32.VirtualFree(reinterpret_cast<void*>(start), len, MEM_DECOMMIT);
  if (prot == PROT_NONE) return true;
  return w32.VirtualAlloc(reinterpret_cast<void*>(start), len, MEM_COMMIT, protection(prot)) != nullptr;
}

struct Sections {
  HANDLE(WINAPI* create)(HANDLE, void*, DWORD, DWORD, DWORD, const WCHAR*);
  void*(WINAPI* map)(HANDLE, DWORD, DWORD, DWORD, size_t, void*);
  BOOL(WINAPI* unmap)(const void*);
  BOOL(WINAPI* flush)(const void*, size_t);
  HANDLE(WINAPI* reopen)(HANDLE, DWORD, DWORD, DWORD);
};
Sections g_sections;
int g_sections_state;

bool sections() {
  const int state = __atomic_load_n(&g_sections_state, __ATOMIC_ACQUIRE);
  if (state) return state > 0;
  void* k = w32_boot->LoadLibraryW(reinterpret_cast<const uint16_t*>(u"kernel32.dll"));
  Sections s{};
  if (k) {
    s.create = reinterpret_cast<decltype(s.create)>(w32_boot->GetProcAddress(k, "CreateFileMappingW"));
    s.map = reinterpret_cast<decltype(s.map)>(w32_boot->GetProcAddress(k, "MapViewOfFileEx"));
    s.unmap = reinterpret_cast<decltype(s.unmap)>(w32_boot->GetProcAddress(k, "UnmapViewOfFile"));
    s.flush = reinterpret_cast<decltype(s.flush)>(w32_boot->GetProcAddress(k, "FlushViewOfFile"));
    s.reopen = reinterpret_cast<decltype(s.reopen)>(w32_boot->GetProcAddress(k, "ReOpenFile"));
  }
  const bool ok = s.create && s.map && s.unmap && s.flush && s.reopen;
  if (ok) g_sections = s;
  __atomic_store_n(&g_sections_state, ok ? 1 : -1, __ATOMIC_RELEASE);
  return ok;
}

// nothing under g_views_lock may malloc, scudo mmaps while holding its own locks (deadlocked once, fun)
enum : uint8_t { kCopy, kView, kSharedCopy };
struct FileView {
  uintptr_t start, end;
  uint64_t offset;
  uint64_t id;
  uintptr_t base;
  size_t size;
  HANDLE file;
  uint64_t* hashes;
  uint8_t kind;
  bool writable;
  char path[446];
};
constexpr size_t kMaxViews = 16384;
FileView* g_views;
size_t g_view_count, g_view_cap;
uint64_t g_view_ids;
int g_shared_copies, g_view_pieces;
W32_SRWLOCK g_views_lock;
W32_SRWLOCK g_copies_lock;  // taken before g_views_lock

bool reserve_views_locked() {
  if (g_view_count < g_view_cap) return true;
  if (g_view_count >= kMaxViews) return false;
  if (!g_views) {
    g_views = static_cast<FileView*>(w32.VirtualAlloc(nullptr, kMaxViews * sizeof(FileView), MEM_RESERVE, PAGE_NOACCESS));
    if (!g_views) return false;
  }
  size_t want = g_view_cap ? g_view_cap * 2 : 64;
  if (want > kMaxViews) want = kMaxViews;
  if (!w32.VirtualAlloc(g_views, want * sizeof(FileView), MEM_COMMIT, PAGE_READWRITE)) return false;
  g_view_cap = want;
  return true;
}

uint64_t new_view_id() { return __atomic_add_fetch(&g_view_ids, 1, __ATOMIC_RELAXED); }

bool add_view(const FileView& v) {
  w32.AcquireSRWLockExclusive(&g_views_lock);
  const bool ok = reserve_views_locked();
  if (ok) {
    g_views[g_view_count++] = v;
    if (v.kind == kSharedCopy) g_shared_copies++;
    if (v.kind == kView) g_view_pieces++;
  }
  w32.ReleaseSRWLockExclusive(&g_views_lock);
  return ok;
}

bool cut_view_locked(size_t i, uintptr_t start, uintptr_t end) {
  FileView& v = g_views[i];
  if (v.start >= start && v.end <= end) {
    if (v.kind == kSharedCopy) g_shared_copies--;
    if (v.kind == kView) g_view_pieces--;
    g_views[i] = g_views[--g_view_count];
    return false;
  }
  if (v.start < start && v.end > end) {
    if (reserve_views_locked()) {
      FileView& tail = g_views[g_view_count++];
      tail = g_views[i];
      tail.offset += end - tail.start;
      tail.start = end;
      if (tail.kind == kSharedCopy) g_shared_copies++;
      if (tail.kind == kView) g_view_pieces++;
    }
    g_views[i].end = start;
  } else if (v.start < start) {
    v.end = start;
  } else {
    v.offset += end - v.start;
    v.start = end;
  }
  return true;
}

bool last_piece_locked(uint64_t id) {
  for (size_t i = 0; i < g_view_count; i++)
    if (g_views[i].id == id) return false;
  return true;
}

void forget_views(uintptr_t start, uintptr_t end) {
  if (__atomic_load_n(&g_view_count, __ATOMIC_RELAXED) == 0) return;
  w32.AcquireSRWLockExclusive(&g_views_lock);
  for (size_t i = 0; i < g_view_count;) {
    const FileView& v = g_views[i];
    if (v.end <= start || v.start >= end) {
      i++;
      continue;
    }
    const uint64_t id = v.id;
    HANDLE file = v.file;
    uint64_t* hashes = v.hashes;
    if (cut_view_locked(i, start, end)) i++;
    if ((file || hashes) && last_piece_locked(id)) {
      if (file) w32.CloseHandle(file);
      if (hashes) w32.VirtualFree(hashes, 0, MEM_RELEASE);
    }
  }
  w32.ReleaseSRWLockExclusive(&g_views_lock);
}

void remember_view(uintptr_t start, uintptr_t end, uint64_t offset, const char* path) {
  FileView v{};
  v.start = start;
  v.end = end;
  v.offset = offset;
  v.id = new_view_id();
  v.kind = kCopy;
  snprintf(v.path, sizeof v.path, "%s", path);
  add_view(v);
}

bool overlaps_shared_copy(uintptr_t start, uintptr_t end) {
  if (__atomic_load_n(&g_shared_copies, __ATOMIC_RELAXED) == 0) return false;
  bool found = false;
  w32.AcquireSRWLockShared(&g_views_lock);
  for (size_t i = 0; i < g_view_count && !found; i++)
    found = g_views[i].kind == kSharedCopy && g_views[i].start < end && g_views[i].end > start;
  w32.ReleaseSRWLockShared(&g_views_lock);
  return found;
}

uint64_t page_hash(uintptr_t page) {
  const uint64_t* w = reinterpret_cast<const uint64_t*>(page);
  uint64_t h = 0x9e3779b97f4a7c15ull;
  for (size_t i = 0; i < kPage / 8; i++) {
    h = (h ^ w[i]) * 0xff51afd7ed558ccdull;
    h ^= h >> 32;
  }
  return h;
}

bool write_run(HANDLE file, uint64_t* hashes, uintptr_t base, uint64_t base_offset, uintptr_t start, uintptr_t end,
               uint64_t size) {
  const uint64_t offset = base_offset + (start - base);
  const size_t n = offset + (end - start) > size ? static_cast<size_t>(size - offset) : end - start;
  for (size_t done = 0; done < n;) {
    W32_OVERLAPPED o{};
    o.offset = static_cast<DWORD>(offset + done);
    o.offset_high = static_cast<DWORD>((offset + done) >> 32);
    const DWORD chunk = n - done < (1u << 30) ? static_cast<DWORD>(n - done) : (1u << 30);
    DWORD wrote = 0;
    if (!w32.WriteFile(file, reinterpret_cast<const void*>(start + done), chunk, &wrote, &o) || wrote == 0) return false;
    done += wrote;
  }
  for (uintptr_t p = start; p < end; p += kPage)
    if (base_offset + (p - base) + kPage <= size) hashes[(p - base) / kPage] = page_hash(p);
  return true;
}

__attribute__((noinline)) bool write_back(HANDLE file, uint64_t* hashes, uintptr_t base, uint64_t base_offset, uintptr_t start, uintptr_t end) {
  int64_t size;
  if (!w32.GetFileSizeEx(file, &size)) return false;
  bool ok = true;
  for_regions(start, end, [&](uintptr_t s, uintptr_t e, const W32_MEMORY_BASIC_INFORMATION& m) {
    if (m.state != MEM_COMMIT) return true;
    DWORD old = 0;
    const bool lift = (m.protect & 0xff) == PAGE_NOACCESS || (m.protect & PAGE_GUARD);
    if (lift && !w32.VirtualProtect(reinterpret_cast<void*>(s), e - s, PAGE_READONLY, &old)) return true;
    uintptr_t run = 0;
    for (uintptr_t p = s;; p += kPage) {
      const bool dirty = p < e && base_offset + (p - base) < static_cast<uint64_t>(size) &&
                         page_hash(p) != hashes[(p - base) / kPage];
      if (dirty && !run) run = p;
      if (!dirty && run) {
        ok = write_run(file, hashes, base, base_offset, run, p, static_cast<uint64_t>(size)) && ok;
        run = 0;
      }
      if (p >= e) break;
    }
    if (lift) w32.VirtualProtect(reinterpret_cast<void*>(s), e - s, old, &old);
    return true;
  });
  return ok;
}

long fill_from_handle(HANDLE h, uintptr_t addr, size_t len, uint64_t offset) {
  for (size_t done = 0; done < len;) {
    W32_OVERLAPPED o{};
    o.offset = static_cast<DWORD>(offset + done);
    o.offset_high = static_cast<DWORD>((offset + done) >> 32);
    const DWORD want = len - done < (1u << 30) ? static_cast<DWORD>(len - done) : (1u << 30);
    DWORD got = 0;
    if (!w32.ReadFile(h, reinterpret_cast<void*>(addr + done), want, &got, &o)) {
      if (w32.GetLastError() == W32_ERROR_HANDLE_EOF) break;
      return w32_fail();
    }
    if (got == 0) break;
    done += got;
  }
  return 0;
}

// GLOBALROOT + the mapped name finds the file even after a rename
constexpr size_t kGlobalRoot = 14;
bool view_file(uintptr_t base, WCHAR* out, size_t cap) {
  memcpy(out, u"\\\\?\\GLOBALROOT", kGlobalRoot * sizeof(WCHAR));
  const DWORD n = w32.K32GetMappedFileNameW(w32.GetCurrentProcess(), reinterpret_cast<void*>(base), out + kGlobalRoot,
                                            static_cast<DWORD>(cap - kGlobalRoot));
  return n > 0 && n < cap - kGlobalRoot - 1;
}

WCHAR g_trash[W32_PATH_MAX];
size_t g_trash_len;
int g_trash_state;

__attribute__((noinline)) bool in_trash(const WCHAR* nt) {
  int state = __atomic_load_n(&g_trash_state, __ATOMIC_ACQUIRE);
  if (!state) {
    WCHAR win[W32_PATH_MAX];
    DWORD n = 0;
    if (w32_path(AT_FDCWD, "/.deleted", win, W32_PATH_MAX) == 0) {
      HANDLE d = w32.CreateFileW(win, 0, FILE_SHARE_ALL, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
      if (d != INVALID_HANDLE_VALUE) {
        n = w32.GetFinalPathNameByHandleW(d, g_trash, W32_PATH_MAX, VOLUME_NAME_NT);
        w32.CloseHandle(d);
      }
    }
    if (n > 0 && n < W32_PATH_MAX) g_trash_len = n;
    state = n > 0 && n < W32_PATH_MAX ? 1 : -1;
    __atomic_store_n(&g_trash_state, state, __ATOMIC_RELEASE);
  }
  return state > 0 && memcmp(nt, g_trash, g_trash_len * sizeof(WCHAR)) == 0 && nt[g_trash_len] == '\\';
}

__attribute__((noinline)) void unmap_view(uintptr_t base) {
  WCHAR name[W32_PATH_MAX];
  const bool trashed = view_file(base, name, W32_PATH_MAX) && in_trash(name + kGlobalRoot);
  g_sections.unmap(reinterpret_cast<void*>(base));
  if (trashed) w32.DeleteFileW(name);  // win7 won't delete a mapped file, so unlink left it in the trash
}

// views keep no handle: an open one blocks renames over the file
__attribute__((noinline)) bool sync_view_file(uintptr_t base) {
  WCHAR name[W32_PATH_MAX];
  if (!view_file(base, name, W32_PATH_MAX)) return true;
  HANDLE h = w32.CreateFileW(name, GENERIC_WRITE, FILE_SHARE_ALL, nullptr, OPEN_EXISTING, 0, nullptr);
  if (h == INVALID_HANDLE_VALUE) return true;
  const bool ok = w32.FlushFileBuffers(h);
  w32.CloseHandle(h);
  return ok;
}

long note_shared_copy(HANDLE h, bool writable, const char* path, uintptr_t start, size_t len, uint64_t offset) {
  FileView v{};
  v.start = start;
  v.end = start + len;
  v.offset = offset;
  v.id = new_view_id();
  v.base = start;
  v.size = len;
  v.kind = kSharedCopy;
  snprintf(v.path, sizeof v.path, "%s", path ? path : "");
  if (writable && sections()) {
    // a dup would share the fd's file pointer, which ftruncate moves
    v.file = g_sections.reopen(h, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_ALL, 0);
    if (v.file == INVALID_HANDLE_VALUE) return w32_fail();
    v.hashes = static_cast<uint64_t*>(w32.VirtualAlloc(nullptr, len / kPage * sizeof(uint64_t), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!v.hashes) {
      w32.CloseHandle(v.file);
      return -ENOMEM;
    }
    for (size_t i = 0; i < len / kPage; i++) v.hashes[i] = page_hash(start + i * kPage);
    v.writable = true;
  }
  if (!add_view(v)) {
    if (v.file) w32.CloseHandle(v.file);
    if (v.hashes) w32.VirtualFree(v.hashes, 0, MEM_RELEASE);
    return -ENOMEM;
  }
  return 0;
}

void forget_names(uintptr_t start, uintptr_t end);

long map_view(HANDLE h, bool rw, const char* path, uintptr_t addr, size_t len, int prot, bool fixed, uint64_t offset) {
  if ((prot & PROT_EXEC) || !sections()) return 0;
  int64_t size;
  if (!w32.GetFileSizeEx(h, &size)) return 0;
  const uint64_t aligned = offset & ~uint64_t{kGranularity - 1}, rem = offset - aligned;
  if (static_cast<uint64_t>(size) <= aligned) return 0;
  const uint64_t avail = static_cast<uint64_t>(size) - aligned;
  if (rem + len > page_up(avail)) return 0;
  const size_t bytes = static_cast<size_t>(rem + len < avail ? rem + len : avail);
  void* want = nullptr;
  if (addr >= rem && addr && !((addr - rem) & (kGranularity - 1))) want = reinterpret_cast<void*>(addr - rem);
  if (fixed && !want) return 0;
  HANDLE s = g_sections.create(h, nullptr, rw ? PAGE_READWRITE : PAGE_READONLY, 0, 0, nullptr);
  if (!s) return 0;
  const DWORD access = rw ? (FILE_MAP_READ | FILE_MAP_WRITE) : FILE_MAP_READ;
  void* p = g_sections.map(s, access, static_cast<DWORD>(aligned >> 32), static_cast<DWORD>(aligned), bytes, want);
  if (!p && want && !fixed) p = g_sections.map(s, access, static_cast<DWORD>(aligned >> 32), static_cast<DWORD>(aligned), bytes, nullptr);
  w32.CloseHandle(s);
  if (!p) return 0;
  const uintptr_t base = reinterpret_cast<uintptr_t>(p), result = base + rem;
  DWORD old;
  if (rem) w32.VirtualProtect(p, rem, PAGE_NOACCESS, &old);
  if (protection(prot) != (rw ? PAGE_READWRITE : PAGE_READONLY) &&
      !w32.VirtualProtect(reinterpret_cast<void*>(result), len, protection(prot), &old)) {
    g_sections.unmap(p);
    return -EACCES;
  }
  forget_views(base, result + len);
  forget_names(result, result + len);
  w32_mem_forget_runs(base, result + len);
  FileView v{};
  v.start = result;
  v.end = result + len;
  v.offset = offset;
  v.id = new_view_id();
  v.base = base;
  v.size = rem + len;
  v.kind = kView;
  v.writable = rw;
  snprintf(v.path, sizeof v.path, "%s", path ? path : "");
  if (!add_view(v)) {
    g_sections.unmap(p);
    return -ENOMEM;
  }
  return static_cast<long>(result);
}

long shared_fd(int fd, int prot, W32File** out) {
  W32File* f = w32_fd_get(fd);
  if (!f) return -EBADF;
  if (f->kind == W32Kind::kFile) {
    const int access = f->flags & O_ACCMODE;
    long r = 0;
    if (f->flags & O_PATH) r = -EBADF;
    else if (access == O_WRONLY || ((prot & PROT_WRITE) && access != O_RDWR)) r = -EACCES;
    if (r < 0) {
      w32_file_put(f);
      return r;
    }
  }
  *out = f;
  return 0;
}

struct Released {
  uintptr_t start, end, base;
  uint64_t base_offset;
  HANDLE file;
  uint64_t* hashes;
  uint8_t kind;
  bool writable, last;
};

void finish_release(const Released& r, uintptr_t* later, int* later_count, int cap) {
  if (r.kind == kView) {
    if (!r.last) return;
    if (later && *later_count < cap) later[(*later_count)++] = r.base;
    else unmap_view(r.base);
  } else if (r.kind == kSharedCopy && r.writable) {
    write_back(r.file, r.hashes, r.base, r.base_offset, r.start, r.end);
    if (r.last) {
      w32.CloseHandle(r.file);
      w32.VirtualFree(r.hashes, 0, MEM_RELEASE);
    }
  }
}

void release_views(uintptr_t start, uintptr_t end, bool views, uintptr_t* later = nullptr, int* later_count = nullptr,
                   int later_cap = 0) {
  if (views ? __atomic_load_n(&g_view_pieces, __ATOMIC_RELAXED) == 0 : __atomic_load_n(&g_view_count, __ATOMIC_RELAXED) == 0) return;
  const bool copies = !views && overlaps_shared_copy(start, end);
  if (copies) w32.AcquireSRWLockExclusive(&g_copies_lock);
  for (bool more = true; more;) {
    Released r[16];
    int n = 0;
    more = false;
    w32.AcquireSRWLockExclusive(&g_views_lock);
    for (size_t i = 0; i < g_view_count;) {
      const FileView& v = g_views[i];
      if (v.end <= start || v.start >= end || (v.kind == kView) != views) {
        i++;
        continue;
      }
      if (n == 16) {
        more = true;
        break;
      }
      Released& x = r[n++];
      x.start = v.start > start ? v.start : start;
      x.end = v.end < end ? v.end : end;
      x.base = v.base;
      x.base_offset = v.offset - (v.start - v.base);
      x.file = v.file;
      x.hashes = v.hashes;
      x.kind = v.kind;
      x.writable = v.writable;
      const uint64_t id = v.id;
      if (cut_view_locked(i, start, end)) i++;
      x.last = last_piece_locked(id);
      DWORD old;
      if (x.kind == kView && !x.last)
        w32.VirtualProtect(reinterpret_cast<void*>(x.start), x.end - x.start, PAGE_NOACCESS, &old);
    }
    w32.ReleaseSRWLockExclusive(&g_views_lock);
    for (int k = 0; k < n; k++) finish_release(r[k], later, later_count, later_cap);
  }
  if (copies) w32.ReleaseSRWLockExclusive(&g_copies_lock);
}

struct ViewPiece {
  uintptr_t start, end;
  uint64_t offset;
};

__attribute__((noinline)) bool view_to_copies(uintptr_t base, size_t size, bool writable, const char* path, const ViewPiece* pieces, int n) {
  struct Protect {
    uintptr_t start, end;
    DWORD protect;
  } protects[32];
  int protect_count = 0;
  size_t total = 0;
  for (int i = 0; i < n; i++) total += pieces[i].end - pieces[i].start;
  auto* keep = total ? static_cast<uint8_t*>(w32.VirtualAlloc(nullptr, total, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)) : nullptr;
  for (size_t i = 0, at = 0; keep && i < static_cast<size_t>(n); i++) {
    const ViewPiece& p = pieces[i];
    for_regions(p.start, p.end, [&](uintptr_t s, uintptr_t e, const W32_MEMORY_BASIC_INFORMATION& m) {
      if (protect_count < 32) protects[protect_count++] = Protect{s, e, m.protect};
      DWORD old;
      if ((m.protect & 0xff) == PAGE_NOACCESS) w32.VirtualProtect(reinterpret_cast<void*>(s), e - s, PAGE_READONLY, &old);
      return true;
    });
    memcpy(keep + at, reinterpret_cast<void*>(p.start), p.end - p.start);
    at += p.end - p.start;
  }
  HANDLE file = nullptr;
  WCHAR name[W32_PATH_MAX];
  if (writable && n && view_file(base, name, W32_PATH_MAX)) {
    file = w32.CreateFileW(name, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_ALL, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) file = nullptr;
  }
  unmap_view(base);
  uint64_t* hashes = nullptr;
  const bool kept = w32.VirtualAlloc(reinterpret_cast<void*>(base), size, MEM_RESERVE, PAGE_NOACCESS) != nullptr;
  if (!kept) {
    w32_debug("mmap: lost the address space of the view at %p to another thread", reinterpret_cast<void*>(base));
    n = 0;
  } else if (file) {
    hashes = static_cast<uint64_t*>(w32.VirtualAlloc(nullptr, size / kPage * sizeof(uint64_t), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
  }
  w32_mem_forget_runs(base, base + size);
  const uint64_t id = new_view_id();
  bool taken = false;
  for (size_t i = 0, at = 0; i < static_cast<size_t>(n); i++) {
    const ViewPiece& p = pieces[i];
    const size_t len = p.end - p.start;
    const bool ok = w32.VirtualAlloc(reinterpret_cast<void*>(p.start), len, MEM_COMMIT, PAGE_READWRITE) != nullptr;
    if (ok && keep) memcpy(reinterpret_cast<void*>(p.start), keep + at, len);
    at += len;
    if (!ok) continue;
    for (uintptr_t q = p.start; hashes && q < p.end; q += kPage) hashes[(q - base) / kPage] = page_hash(q);
    FileView v{};
    v.start = p.start;
    v.end = p.end;
    v.offset = p.offset;
    v.id = id;
    v.base = base;
    v.size = size;
    v.file = hashes ? file : nullptr;
    v.hashes = hashes;
    v.kind = kSharedCopy;
    v.writable = hashes != nullptr;
    memcpy(v.path, path, sizeof v.path);
    if (add_view(v)) taken = true;
  }
  for (int i = 0; i < protect_count && n; i++) {
    DWORD old;
    w32.VirtualProtect(reinterpret_cast<void*>(protects[i].start), protects[i].end - protects[i].start, protects[i].protect, &old);
  }
  if (keep) w32.VirtualFree(keep, 0, MEM_RELEASE);
  if (!taken || !hashes) {
    if (file) w32.CloseHandle(file);
    if (hashes) w32.VirtualFree(hashes, 0, MEM_RELEASE);
  }
  return kept;
}

// windows can't put pages into a view, so MAP_FIXED over one turns it into copies
bool vacate_views(uintptr_t start, uintptr_t end) {
  for (;;) {
    if (__atomic_load_n(&g_view_pieces, __ATOMIC_RELAXED) == 0) return true;
    ViewPiece pieces[16];
    int n = 0, dropped = 0;
    uintptr_t base = 0;
    size_t size = 0;
    bool writable = false;
    char path[sizeof(FileView::path)];
    w32.AcquireSRWLockExclusive(&g_views_lock);
    for (size_t i = 0; i < g_view_count && !base; i++) {
      const FileView& v = g_views[i];
      if (v.kind == kView && v.base < end && v.base + v.size > start) {
        base = v.base;
        size = v.size;
        writable = v.writable;
        memcpy(path, v.path, sizeof path);
      }
    }
    for (size_t i = 0; base && i < g_view_count;) {
      if (g_views[i].kind != kView || g_views[i].base != base) {
        i++;
        continue;
      }
      if (n < 16) pieces[n++] = ViewPiece{g_views[i].start, g_views[i].end, g_views[i].offset};
      else dropped++;
      g_views[i] = g_views[--g_view_count];
      g_view_pieces--;
    }
    w32.ReleaseSRWLockExclusive(&g_views_lock);
    if (!base) return true;
    if (dropped) w32_debug("mmap: the view at %p was in more than 16 pieces; %d are lost", reinterpret_cast<void*>(base), dropped);
    if (!view_to_copies(base, size, writable, path, pieces, n)) return false;
  }
}

struct AnonName {
  uintptr_t start, end;
  char name[80];
};
constexpr size_t kMaxNames = 65536;
AnonName* g_names;
size_t g_name_count, g_name_cap;
W32_SRWLOCK g_names_lock;

bool reserve_names_locked(size_t more) {
  if (g_name_count + more <= g_name_cap) return true;
  if (g_name_count + more > kMaxNames) return false;
  if (!g_names) {
    g_names = static_cast<AnonName*>(w32.VirtualAlloc(nullptr, kMaxNames * sizeof(AnonName), MEM_RESERVE, PAGE_NOACCESS));
    if (!g_names) return false;
  }
  size_t want = g_name_cap ? g_name_cap * 2 : 64;
  while (want < g_name_count + more) want *= 2;
  if (want > kMaxNames) want = kMaxNames;
  if (!w32.VirtualAlloc(g_names, want * sizeof(AnonName), MEM_COMMIT, 0x04 /* PAGE_READWRITE */)) return false;
  g_name_cap = want;
  return true;
}

void forget_names_locked(uintptr_t start, uintptr_t end) {
  for (size_t i = 0; i < g_name_count;) {
    AnonName& v = g_names[i];
    if (v.end <= start || v.start >= end) {
      i++;
    } else if (v.start >= start && v.end <= end) {
      g_names[i] = g_names[--g_name_count];
    } else if (v.start < start && v.end > end) {
      if (reserve_names_locked(1)) {
        g_names[g_name_count] = g_names[i];
        g_names[g_name_count].start = end;
        g_name_count++;
      }
      g_names[i].end = start;
      i++;
    } else if (v.start < start) {
      v.end = start;
      i++;
    } else {
      v.start = end;
      i++;
    }
  }
}

void forget_names(uintptr_t start, uintptr_t end) {
  if (__atomic_load_n(&g_name_count, __ATOMIC_RELAXED) == 0) return;
  w32.AcquireSRWLockExclusive(&g_names_lock);
  forget_names_locked(start, end);
  w32.ReleaseSRWLockExclusive(&g_names_lock);
}

void add_name_locked(uintptr_t start, uintptr_t end, const char* name) {
  if (start >= end || !reserve_names_locked(1)) return;
  AnonName& n = g_names[g_name_count++];
  n.start = start;
  n.end = end;
  snprintf(n.name, sizeof n.name, "%s", name);
}

bool overlaps_view(uintptr_t start, uintptr_t end) {
  bool found = false;
  w32.AcquireSRWLockExclusive(&g_views_lock);
  for (size_t i = 0; i < g_view_count && !found; i++) found = g_views[i].start < end && g_views[i].end > start;
  w32.ReleaseSRWLockExclusive(&g_views_lock);
  return found;
}

long fill_from_file(uintptr_t addr, size_t len, int fd, off_t offset) {
  size_t done = 0;
  while (done < len) {
    ssize_t n = pread64(fd, reinterpret_cast<void*>(addr + done), len - done, offset + static_cast<off_t>(done));
    if (n < 0) return -errno;
    if (n == 0) break;
    done += static_cast<size_t>(n);
  }
  return 0;
}

long copy_shared(HANDLE h, bool writable, const char* path, size_t len, int prot, uint64_t offset) {
  void* p = w32.VirtualAlloc(nullptr, len, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
  if (!p) return -ENOMEM;
  const uintptr_t a = reinterpret_cast<uintptr_t>(p);
  forget_views(a, a + len);
  forget_names(a, a + len);
  long r = fill_from_handle(h, a, len, offset);
  if (r == 0) r = note_shared_copy(h, writable, path, a, len, offset);
  DWORD old;
  if (r == 0 && protection(prot) != PAGE_READWRITE && !w32.VirtualProtect(p, len, protection(prot), &old)) r = w32_fail();
  if (r < 0) {
    sys_munmap(static_cast<long>(a), static_cast<long>(len), 0, 0, 0, 0);
    return r;
  }
  set_run(a, a + len, a, protection(prot), MEM_COMMIT);
  return static_cast<long>(a);
}

constexpr long kNotShared = 1;

__attribute__((noinline)) long remap_shared(uintptr_t old_addr, size_t old_len, size_t new_len, long flags) {
  FileView v;
  bool found = false;
  w32.AcquireSRWLockShared(&g_views_lock);
  for (size_t i = 0; i < g_view_count && !found; i++) {
    const FileView& p = g_views[i];
    if (p.start <= old_addr && old_addr < p.end && (p.kind == kView || (p.kind == kSharedCopy && p.writable))) {
      v = p;
      found = true;
    }
  }
  w32.ReleaseSRWLockShared(&g_views_lock);
  if (!found) return kNotShared;
  if (old_addr + old_len > v.end) return -EFAULT;
  if (new_len <= old_len) {
    if (new_len < old_len) sys_munmap(static_cast<long>(old_addr + new_len), static_cast<long>(old_len - new_len), 0, 0, 0, 0);
    return static_cast<long>(old_addr);
  }
  if (!(flags & MREMAP_MAYMOVE)) return -ENOMEM;
  W32_MEMORY_BASIC_INFORMATION m;
  const int prot = w32.VirtualQuery(reinterpret_cast<void*>(old_addr), &m, sizeof m) ? prot_of(m.protect) : (PROT_READ | PROT_WRITE);
  const uint64_t offset = v.offset + (old_addr - v.start);
  HANDLE h = INVALID_HANDLE_VALUE;
  if (v.kind == kView) {
    WCHAR name[W32_PATH_MAX];
    if (view_file(v.base, name, W32_PATH_MAX))
      h = w32.CreateFileW(name, GENERIC_READ | (v.writable ? GENERIC_WRITE : 0), FILE_SHARE_ALL, nullptr, OPEN_EXISTING, 0, nullptr);
  } else {
    sys_msync(static_cast<long>(old_addr), static_cast<long>(old_len), MS_ASYNC, 0, 0, 0);
    w32.AcquireSRWLockExclusive(&g_copies_lock);
    if (sections()) h = g_sections.reopen(v.file, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_ALL, 0);
    w32.ReleaseSRWLockExclusive(&g_copies_lock);
  }
  if (h == INVALID_HANDLE_VALUE) return -ENOMEM;
  long r = map_view(h, v.writable, v.path, 0, new_len, prot, false, offset);
  if (r == 0) r = copy_shared(h, v.writable, v.path, new_len, prot, offset);
  w32.CloseHandle(h);
  if (r < 0) return r;
  sys_munmap(static_cast<long>(old_addr), static_cast<long>(old_len), 0, 0, 0, 0);
  return r;
}

bool all_mapped(uintptr_t start, uintptr_t end) {
  for (uintptr_t a = start; a < end;) {
    uintptr_t next = 0;
    bool ours = false;
    W32_MEMORY_BASIC_INFORMATION m;
    if (!w32.VirtualQuery(reinterpret_cast<void*>(a), &m, sizeof m) || m.state == MEM_FREE) return false;
    if (__atomic_load_n(&g_view_count, __ATOMIC_RELAXED)) {
      w32.AcquireSRWLockShared(&g_views_lock);
      for (size_t i = 0; i < g_view_count; i++) {
        const FileView& v = g_views[i];
        if (v.start <= a && a < v.end && v.end > next) next = v.end;
        if (v.kind == kView && v.base == reinterpret_cast<uintptr_t>(m.allocation_base)) ours = true;
      }
      w32.ReleaseSRWLockShared(&g_views_lock);
    }
    if (!next) {
      if (ours && m.type == MEM_MAPPED) return false;
      next = reinterpret_cast<uintptr_t>(m.base) + m.region_size;
    }
    a = next;
  }
  return true;
}

__attribute__((destructor)) void write_back_at_exit() {
  if (__atomic_load_n(&g_shared_copies, __ATOMIC_RELAXED) == 0) return;
  w32.AcquireSRWLockExclusive(&g_copies_lock);
  for (size_t next = 0;;) {
    Released r[16];
    int n = 0;
    w32.AcquireSRWLockShared(&g_views_lock);
    size_t i = next;
    for (; i < g_view_count && n < 16; i++) {
      const FileView& v = g_views[i];
      if (v.kind == kSharedCopy && v.writable)
        r[n++] = Released{v.start, v.end, v.base, v.offset - (v.start - v.base), v.file, v.hashes, v.kind, true, false};
    }
    const bool more = i < g_view_count;
    next = i;
    w32.ReleaseSRWLockShared(&g_views_lock);
    for (int k = 0; k < n; k++) write_back(r[k].file, r[k].hashes, r[k].base, r[k].base_offset, r[k].start, r[k].end);
    if (!more) break;
  }
  w32.ReleaseSRWLockExclusive(&g_copies_lock);
}

void advise(uintptr_t addr, uintptr_t end, long advice) {
  if (advice == MADV_DONTNEED || advice == MADV_REMOVE) {
    // linux hands out zero pages on the next touch: decommit and commit again
    DWORD protect;
    if (cached_run(addr, end, &protect)) {
      w32.VirtualFree(reinterpret_cast<void*>(addr), end - addr, MEM_DECOMMIT);
      w32.VirtualAlloc(reinterpret_cast<void*>(addr), end - addr, MEM_COMMIT, protect);
      return;
    }
    const uint64_t seen = run_generation();
    for_regions(addr, end, [seen](uintptr_t s, uintptr_t e, const W32_MEMORY_BASIC_INFORMATION& m) {
      if (m.state == MEM_COMMIT && m.type != MEM_MAPPED) {
        w32.VirtualFree(reinterpret_cast<void*>(s), e - s, MEM_DECOMMIT);
        w32.VirtualAlloc(reinterpret_cast<void*>(s), e - s, MEM_COMMIT, m.protect);
        if (m.type == MEM_PRIVATE)
          remember_run(s, reinterpret_cast<uintptr_t>(m.base) + m.region_size,
                       reinterpret_cast<uintptr_t>(m.allocation_base), m.protect, seen);
      }
      return true;
    });
  } else if (advice == MADV_FREE) {
    DWORD protect;
    if (cached_run(addr, end, &protect)) {
      if (protect != PAGE_NOACCESS) w32.VirtualAlloc(reinterpret_cast<void*>(addr), end - addr, MEM_RESET, protect);
      return;
    }
    const uint64_t seen = run_generation();
    for_regions(addr, end, [seen](uintptr_t s, uintptr_t e, const W32_MEMORY_BASIC_INFORMATION& m) {
      if (m.state == MEM_COMMIT && m.type != MEM_MAPPED) {
        if (m.protect != PAGE_NOACCESS) w32.VirtualAlloc(reinterpret_cast<void*>(s), e - s, MEM_RESET, m.protect);
        if (m.type == MEM_PRIVATE)
          remember_run(s, reinterpret_cast<uintptr_t>(m.base) + m.region_size,
                       reinterpret_cast<uintptr_t>(m.allocation_base), m.protect, seen);
      }
      return true;
    });
  }
}

}  // namespace

void w32_mem_note_run(uintptr_t start, uintptr_t end, uintptr_t allocation, DWORD protect) {
  set_run(start, end, allocation, protect, MEM_COMMIT);
}

void w32_mem_forget_runs(uintptr_t start, uintptr_t end) {
  w32.AcquireSRWLockExclusive(&g_runs_lock);
  forget_runs_locked(start, end);
  w32.ReleaseSRWLockExclusive(&g_runs_lock);
}

bool w32_mem_run_at(uintptr_t a, uintptr_t* end, DWORD* protect, DWORD* state) {
  w32.AcquireSRWLockShared(&g_runs_lock);
  const Run* r = find_run_locked(a, a + 1);
  if (r) {
    *end = r->end;
    *protect = r->protect;
    *state = r->state;
  }
  w32.ReleaseSRWLockShared(&g_runs_lock);
  return r != nullptr;
}

long sys_mmap(long addr_l, long length, long prot_l, long flags_l, long fd_l, long offset) {
  uintptr_t addr = static_cast<uintptr_t>(addr_l);
  int prot = static_cast<int>(prot_l), flags = static_cast<int>(flags_l), fd = static_cast<int>(fd_l);
  if (length <= 0 || (offset & (kPage - 1))) return -EINVAL;
  size_t len = page_up(static_cast<size_t>(length));
  bool anonymous = flags & MAP_ANONYMOUS;
  bool fixed = flags & (MAP_FIXED | MAP_FIXED_NOREPLACE);
  if (fixed && (addr & (kPage - 1))) return -EINVAL;
  W32File* shared = nullptr;
  if (!anonymous && ((flags & MAP_TYPE) == MAP_SHARED || (flags & MAP_TYPE) == MAP_SHARED_VALIDATE)) {
    const long r = shared_fd(fd, prot, &shared);
    if (r < 0) return r;
  }
  FilePut put{shared};
  const bool shared_file = shared && shared->kind == W32Kind::kFile;
  const bool rw = shared_file && (shared->flags & O_ACCMODE) == O_RDWR;
  if (fixed && !(flags & MAP_FIXED_NOREPLACE)) {
    if (!vacate_views(addr, addr + len)) return -ENOMEM;
    release_views(addr, addr + len, false);
  }
  if (shared_file) {
    const long r = map_view(shared->handle, rw, shared->upath, addr, len, prot, fixed, static_cast<uint64_t>(offset));
    if (w32_log_wanted())
      w32_debug("mmap: MAP_SHARED %s, %zu bytes at %ld, prot %d: %s", shared->upath ? shared->upath : "?", len, offset, prot,
                r > 0 ? "view" : r == 0 ? "copy" : strerror(static_cast<int>(-r)));
    if (r != 0) return r;
  }
  int initial_prot = anonymous ? prot : (prot | PROT_READ | PROT_WRITE);
  uintptr_t result = 0;
  uintptr_t allocation = 0;
  ForgetRuns forget{0, 0};

  if (fixed) {
    forget.start = addr;
    forget.end = addr + len;
    if (known_reservation(addr, addr + len, &allocation)) {
      if (flags & MAP_FIXED_NOREPLACE) return -EEXIST;
      if (!replace_pages(addr, addr + len, initial_prot)) return w32_fail();
    } else if (all_free(addr, addr + len)) {
      if (addr & (kGranularity - 1)) return -ENOMEM;
      DWORD type = initial_prot == PROT_NONE ? MEM_RESERVE : (MEM_RESERVE | MEM_COMMIT);
      if (!w32.VirtualAlloc(reinterpret_cast<void*>(addr), len, type, protection(initial_prot))) return -ENOMEM;
      allocation = addr;
    } else if (none_free_in(addr, addr + len, &allocation)) {
      if (flags & MAP_FIXED_NOREPLACE) return -EEXIST;
      if (!replace_pages(addr, addr + len, initial_prot)) return w32_fail();
    } else {
      return -ENOMEM;
    }
    result = addr;
  } else {
    DWORD type = initial_prot == PROT_NONE ? MEM_RESERVE : (MEM_RESERVE | MEM_COMMIT);
    void* p = nullptr;
    if (addr && !(addr & (kGranularity - 1))) p = w32.VirtualAlloc(reinterpret_cast<void*>(addr), len, type, protection(initial_prot));
    if (!p) p = w32.VirtualAlloc(nullptr, len, type, protection(initial_prot));
    if (!p) return -ENOMEM;
    result = reinterpret_cast<uintptr_t>(p);
    allocation = result;
    forget.start = result;
    forget.end = result + len;
  }

  if (!fixed || (flags & MAP_FIXED_NOREPLACE)) forget_views(result, result + len);
  forget_names(result, result + len);
  if (!anonymous) {
    long r = fill_from_file(result, static_cast<size_t>(length), fd, static_cast<off_t>(offset));
    if (r == 0 && shared_file) r = note_shared_copy(shared->handle, rw, shared->upath, result, len, static_cast<uint64_t>(offset));
    DWORD old;
    if (r == 0 && initial_prot != prot && !w32.VirtualProtect(reinterpret_cast<void*>(result), len, protection(prot), &old)) r = w32_fail();
    if (r < 0) {
      sys_munmap(static_cast<long>(result), static_cast<long>(len), 0, 0, 0, 0);
      return r;
    }
    if (prot & PROT_EXEC) w32.FlushInstructionCache(w32.GetCurrentProcess(), reinterpret_cast<void*>(result), len);
    if (!shared_file) {
      if (W32File* f = w32_fd_get(fd)) {
        if (f->upath) remember_view(result, result + len, static_cast<uint64_t>(offset), f->upath);
        w32_file_put(f);
      }
    }
  }
  if (prot == PROT_NONE && anonymous) set_run(result, result + len, allocation, 0, MEM_RESERVE);
  else set_run(result, result + len, allocation, protection(prot), MEM_COMMIT);
  forget.start = forget.end = 0;
  return static_cast<long>(result);
}

long sys_munmap(long addr_l, long length, long, long, long, long) {
  uintptr_t addr = static_cast<uintptr_t>(addr_l);
  if ((addr & (kPage - 1)) || length <= 0) return -EINVAL;
  uintptr_t end = addr + page_up(static_cast<size_t>(length));
  ForgetRuns forget{addr, end};
  uintptr_t later[64];
  int later_count = 0;
  release_views(addr, end, false);
  release_views(addr, end, true, later, &later_count, 64);
  forget_names(addr, end);
  struct Part {
    uintptr_t start, end, allocation;
  } reserved[16];
  int reserved_count = 0;
  uintptr_t a = addr;
  while (a < end) {
    W32_MEMORY_BASIC_INFORMATION mbi;
    if (!w32.VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof(mbi))) break;
    uintptr_t rend = reinterpret_cast<uintptr_t>(mbi.base) + mbi.region_size;
    if (mbi.state == MEM_FREE || mbi.type == MEM_MAPPED) {
      a = rend;
      continue;
    }
    uintptr_t alloc = reinterpret_cast<uintptr_t>(mbi.allocation_base);
    uintptr_t alloc_end = allocation_end(mbi.allocation_base);
    if (alloc >= addr && alloc_end <= end) {
      w32.VirtualFree(mbi.allocation_base, 0, MEM_RELEASE);
      a = alloc_end;
    } else {
      uintptr_t e = alloc_end < end ? alloc_end : end;
      w32.VirtualFree(reinterpret_cast<void*>(a), e - a, MEM_DECOMMIT);
      if (reserved_count < 16) reserved[reserved_count++] = Part{a, e, alloc};
      a = e;
    }
  }
  w32_mem_forget_runs(addr, end);
  for (int i = 0; i < reserved_count; i++) set_run(reserved[i].start, reserved[i].end, reserved[i].allocation, 0, MEM_RESERVE);
  // views last: once one's unmapped another thread can grab the hole
  for (int i = 0; i < later_count; i++) unmap_view(later[i]);
  forget.start = forget.end = 0;
  return 0;
}

long sys_mprotect(long addr_l, long length, long prot_l, long, long, long) {
  uintptr_t addr = static_cast<uintptr_t>(addr_l);
  int prot = static_cast<int>(prot_l);
  if (addr & (kPage - 1)) return -EINVAL;
  if (length == 0) return 0;
  uintptr_t end = addr + page_up(static_cast<size_t>(length));
  ForgetRuns forget{addr, end};
  long result = 0;
  struct Piece {
    uintptr_t start, end, allocation;
    bool committed;
    bool view;
  } pieces[16];
  int piece_count = 0;
  bool all_pieces = true;
  for_regions(addr, end, [&](uintptr_t s, uintptr_t e, const W32_MEMORY_BASIC_INFORMATION& m) {
    DWORD old;
    if (m.state == MEM_FREE) {
      result = -ENOMEM;
      return false;
    }
    const bool committed = m.state == MEM_COMMIT || prot != PROT_NONE;
    if (m.state == MEM_RESERVE) {
      if (prot != PROT_NONE && !w32.VirtualAlloc(reinterpret_cast<void*>(s), e - s, MEM_COMMIT, protection(prot))) {
        result = -ENOMEM;
        return false;
      }
    } else if (!w32.VirtualProtect(reinterpret_cast<void*>(s), e - s, protection(prot), &old)) {
      result = m.type == MEM_MAPPED ? -EACCES : w32_fail();
      return false;
    }
    if (piece_count < 16) {
      pieces[piece_count++] = Piece{s, e, reinterpret_cast<uintptr_t>(m.allocation_base), committed, m.type == MEM_MAPPED};
    } else {
      all_pieces = false;
    }
    return true;
  });
  if (result == 0 && (prot & PROT_EXEC)) w32.FlushInstructionCache(w32.GetCurrentProcess(), reinterpret_cast<void*>(addr), end - addr);
  if (result == 0 && all_pieces) {
    w32_mem_forget_runs(addr, end);
    for (int i = 0; i < piece_count; i++) {
      if (pieces[i].view) continue;
      set_run(pieces[i].start, pieces[i].end, pieces[i].allocation, pieces[i].committed ? protection(prot) : 0,
              pieces[i].committed ? MEM_COMMIT : MEM_RESERVE);
    }
    forget.start = forget.end = 0;
  }
  return result;
}

long sys_madvise(long addr_l, long length, long advice, long, long, long) {
  uintptr_t addr = static_cast<uintptr_t>(addr_l);
  if (addr & (kPage - 1)) return -EINVAL;
  if (length == 0) return 0;
  uintptr_t end = addr + page_up(static_cast<size_t>(length));
  if (__builtin_expect(g_w32_stats, 0)) w32_stats_madvise(static_cast<int>(advice), end - addr);
  if (advice != MADV_DONTNEED && advice != MADV_REMOVE && advice != MADV_FREE) return 0;
  while (addr < end && __atomic_load_n(&g_shared_copies, __ATOMIC_RELAXED)) {
    uintptr_t s = end, e = end;
    w32.AcquireSRWLockShared(&g_views_lock);
    for (size_t i = 0; i < g_view_count; i++) {
      const FileView& v = g_views[i];
      const uintptr_t vs = v.start > addr ? v.start : addr;
      if (v.kind == kSharedCopy && v.end > addr && v.start < end && vs < s) {
        s = vs;
        e = v.end < end ? v.end : end;
      }
    }
    w32.ReleaseSRWLockShared(&g_views_lock);
    if (s > addr) advise(addr, s, advice);
    addr = e;
  }
  if (addr < end) advise(addr, end, advice);
  return 0;
}

long sys_mremap(long old_l, long old_size, long new_size, long flags, long new_addr, long) {
  uintptr_t old_addr = static_cast<uintptr_t>(old_l);
  size_t old_len = page_up(static_cast<size_t>(old_size)), new_len = page_up(static_cast<size_t>(new_size));
  if ((old_addr & (kPage - 1)) || new_len == 0) return -EINVAL;
  if (flags & MREMAP_FIXED) return -EINVAL;
  ForgetRuns forget{old_addr, old_addr + (new_len > old_len ? new_len : old_len)};
  if (__atomic_load_n(&g_view_count, __ATOMIC_RELAXED)) {
    const long r = remap_shared(old_addr, old_len, new_len, flags);
    if (r != kNotShared) return r;
  }
  if (new_len <= old_len) {
    if (new_len < old_len) sys_munmap(static_cast<long>(old_addr + new_len), static_cast<long>(old_len - new_len), 0, 0, 0, 0);
    return static_cast<long>(old_addr);
  }
  W32_MEMORY_BASIC_INFORMATION mbi;
  if (!w32.VirtualQuery(reinterpret_cast<void*>(old_addr), &mbi, sizeof(mbi))) return -EFAULT;
  W32_MEMORY_BASIC_INFORMATION next;
  if (w32.VirtualQuery(reinterpret_cast<void*>(old_addr + old_len), &next, sizeof(next)) && next.state == MEM_RESERVE &&
      next.allocation_base == mbi.allocation_base &&
      reinterpret_cast<uintptr_t>(next.base) + next.region_size >= old_addr + new_len &&
      w32.VirtualAlloc(reinterpret_cast<void*>(old_addr + old_len), new_len - old_len, MEM_COMMIT, mbi.protect)) {
    w32.AcquireSRWLockExclusive(&g_names_lock);
    for (size_t i = 0; i < g_name_count; i++)
      if (g_names[i].end == old_addr + old_len && g_names[i].start >= old_addr) g_names[i].end = old_addr + new_len;
    w32.ReleaseSRWLockExclusive(&g_names_lock);
    return static_cast<long>(old_addr);
  }
  if (!(flags & MREMAP_MAYMOVE)) return -ENOMEM;
  void* p = w32.VirtualAlloc(nullptr, new_len, MEM_RESERVE | MEM_COMMIT, mbi.protect == PAGE_NOACCESS ? 0x04 : mbi.protect);
  if (!p) return -ENOMEM;
  w32_mem_forget_runs(reinterpret_cast<uintptr_t>(p), reinterpret_cast<uintptr_t>(p) + new_len);
  memcpy(p, reinterpret_cast<void*>(old_addr), old_len);
  const uintptr_t moved = reinterpret_cast<uintptr_t>(p);
  w32.AcquireSRWLockExclusive(&g_names_lock);
  for (size_t i = 0, n = g_name_count; i < n; i++) {
    const AnonName v = g_names[i];
    const uintptr_t s = v.start > old_addr ? v.start : old_addr, e = v.end < old_addr + old_len ? v.end : old_addr + old_len;
    if (s < e) add_name_locked(moved + (s - old_addr), moved + (e - old_addr) + (e == old_addr + old_len ? new_len - old_len : 0), v.name);
  }
  w32.ReleaseSRWLockExclusive(&g_names_lock);
  sys_munmap(static_cast<long>(old_addr), static_cast<long>(old_len), 0, 0, 0, 0);
  (void)new_addr;
  return reinterpret_cast<long>(p);
}

long sys_msync(long addr_l, long length, long flags_l, long, long, long) {
  const uintptr_t addr = static_cast<uintptr_t>(addr_l);
  const int flags = static_cast<int>(flags_l);
  if ((addr & (kPage - 1)) || (flags & ~(MS_ASYNC | MS_INVALIDATE | MS_SYNC)) || ((flags & MS_ASYNC) && (flags & MS_SYNC)))
    return -EINVAL;
  const uintptr_t end = addr + page_up(static_cast<size_t>(length));
  if (end < addr || !all_mapped(addr, end)) return -ENOMEM;
  if (__atomic_load_n(&g_view_count, __ATOMIC_RELAXED) == 0) return 0;
  const bool copies = overlaps_shared_copy(addr, end);
  if (copies) w32.AcquireSRWLockExclusive(&g_copies_lock);
  bool ok = true;
  uintptr_t synced[8];
  int synced_count = 0;
  for (size_t next = 0;;) {
    Released r[16];
    int n = 0;
    w32.AcquireSRWLockShared(&g_views_lock);
    size_t i = next;
    for (; i < g_view_count && n < 16; i++) {
      const FileView& v = g_views[i];
      if (v.end <= addr || v.start >= end || v.kind == kCopy || !v.writable) continue;
      r[n++] = Released{v.start > addr ? v.start : addr, v.end < end ? v.end : end, v.base, v.offset - (v.start - v.base),
                        v.file, v.hashes, v.kind, true, false};
    }
    const bool more = i < g_view_count;
    next = i;
    w32.ReleaseSRWLockShared(&g_views_lock);
    for (int k = 0; k < n; k++) {
      if (r[k].kind == kView) {
        ok = g_sections.flush(reinterpret_cast<void*>(r[k].start), r[k].end - r[k].start) && ok;
        bool done = false;
        for (int j = 0; j < synced_count; j++) done = done || synced[j] == r[k].base;
        if ((flags & MS_SYNC) && !done) {
          ok = sync_view_file(r[k].base) && ok;
          if (synced_count < 8) synced[synced_count++] = r[k].base;
        }
      } else {
        ok = write_back(r[k].file, r[k].hashes, r[k].base, r[k].base_offset, r[k].start, r[k].end) && ok;
        if (flags & MS_SYNC) ok = w32.FlushFileBuffers(r[k].file) && ok;
      }
    }
    if (!more) break;
  }
  if (copies) w32.ReleaseSRWLockExclusive(&g_copies_lock);
  return ok ? 0 : -EIO;
}

long sys_mlock(long, long, long, long, long, long) { return 0; }
long sys_mlock2(long, long, long, long, long, long) { return 0; }
long sys_munlock(long, long, long, long, long, long) { return 0; }
long sys_mlockall(long, long, long, long, long, long) { return 0; }
long sys_munlockall(long, long, long, long, long, long) { return 0; }
long sys_brk(long, long, long, long, long, long) { return 0; }

long sys_mincore(long addr, long length, long vec, long, long, long) {
  size_t pages = page_up(static_cast<size_t>(length)) / kPage;
  memset(reinterpret_cast<void*>(vec), 1, pages);
  (void)addr;
  return 0;
}

long w32_mem_set_anon_name(uintptr_t start, size_t length, const char* name) {
  if (start & (kPage - 1)) return -EINVAL;
  const uintptr_t end = start + page_up(length);
  if (end < start) return -EINVAL;
  if (name) {
    size_t n = 0;
    for (; n < 80 && name[n]; n++) {
      const unsigned char c = static_cast<unsigned char>(name[n]);
      if (c < 0x20 || c > 0x7e || c == '\\' || c == '`' || c == '$' || c == '[' || c == ']') return -EINVAL;
    }
    if (n == 80) return -EINVAL;
  }
  if (end == start) return 0;
  if (overlaps_view(start, end)) return -EBADF;
  long result = 0;
  w32.AcquireSRWLockExclusive(&g_names_lock);
  forget_names_locked(start, end);
  for_regions(start, end, [&](uintptr_t s, uintptr_t e, const W32_MEMORY_BASIC_INFORMATION& m) {
    if (m.state == MEM_FREE) result = -ENOMEM;
    else if (name && *name) add_name_locked(s, e, name);
    return true;
  });
  w32.ReleaseSRWLockExclusive(&g_names_lock);
  return result;
}

size_t w32_mem_anon_names(W32AnonName* out, size_t cap) {
  w32.AcquireSRWLockExclusive(&g_names_lock);
  size_t n = g_name_count;
  for (size_t i = 0; i < n && i < cap; i++) {
    out[i].start = g_names[i].start;
    out[i].end = g_names[i].end;
    memcpy(out[i].name, g_names[i].name, sizeof out[i].name);
  }
  w32.ReleaseSRWLockExclusive(&g_names_lock);
  return n;
}

size_t w32_mem_file_views(W32FileView* out, size_t cap) {
  w32.AcquireSRWLockExclusive(&g_views_lock);
  size_t n = g_view_count;
  for (size_t i = 0; i < n && i < cap; i++) {
    out[i].start = g_views[i].start;
    out[i].end = g_views[i].end;
    out[i].offset = g_views[i].offset;
    snprintf(out[i].path, sizeof out[i].path, "%s", g_views[i].path);
  }
  w32.ReleaseSRWLockExclusive(&g_views_lock);
  return n;
}

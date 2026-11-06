// our own dynamic linker. symbol versions? never heard of them

#include <dlfcn.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <link.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "w32_kernel.h"

namespace {

constexpr uintptr_t kPage = 4096;
constexpr int kHost = 0, kGuest = 1;

struct SymMap {
  char** names;
  void** addrs;
  size_t count, cap;
  uint32_t* slots;
  size_t nslots;
};

uint32_t gnu_hash_of(const char* s) {
  uint32_t h = 5381;
  for (; *s; s++) h = h * 33 + static_cast<uint8_t>(*s);
  return h;
}

void* map_find(const SymMap* m, const char* name, uint32_t h) {
  if (!m || !m->nslots) return nullptr;
  for (size_t i = h & (m->nslots - 1);; i = (i + 1) & (m->nslots - 1)) {
    uint32_t s = m->slots[i];
    if (!s) return nullptr;
    if (strcmp(m->names[s - 1], name) == 0) return m->addrs[s - 1];
  }
}

void map_put(SymMap* m, const char* name, void* addr) {
  uint32_t h = gnu_hash_of(name);
  if (m->nslots) {
    for (size_t i = h & (m->nslots - 1);; i = (i + 1) & (m->nslots - 1)) {
      uint32_t s = m->slots[i];
      if (!s) break;
      if (strcmp(m->names[s - 1], name) == 0) {
        m->addrs[s - 1] = addr;
        return;
      }
    }
  }
  if (m->count == m->cap) {
    m->cap = m->cap ? m->cap * 2 : 64;
    m->names = static_cast<char**>(realloc(m->names, m->cap * sizeof(char*)));
    m->addrs = static_cast<void**>(realloc(m->addrs, m->cap * sizeof(void*)));
  }
  m->names[m->count] = strdup(name);
  m->addrs[m->count] = addr;
  m->count++;
  if (m->count * 2 > m->nslots) {
    free(m->slots);
    m->nslots = m->nslots ? m->nslots * 2 : 128;
    while (m->count * 2 > m->nslots) m->nslots *= 2;
    m->slots = static_cast<uint32_t*>(calloc(m->nslots, sizeof(uint32_t)));
    for (size_t k = 0; k < m->count; k++) {
      size_t i = gnu_hash_of(m->names[k]) & (m->nslots - 1);
      while (m->slots[i]) i = (i + 1) & (m->nslots - 1);
      m->slots[i] = static_cast<uint32_t>(k + 1);
    }
  } else {
    size_t i = h & (m->nslots - 1);
    while (m->slots[i]) i = (i + 1) & (m->nslots - 1);
    m->slots[i] = static_cast<uint32_t>(m->count);
  }
}

struct Object {
  Object* next;
  int ns;
  char* name;
  char* path;
  uintptr_t base;
  uintptr_t map_start, map_size;
  const ElfW(Phdr)* phdr;
  ElfW(Half) phnum;
  const ElfW(Dyn)* dynamic;
  const char* strtab;
  const ElfW(Sym)* symtab;
  const uint32_t* gnu_hash;
  const uint32_t* sysv_hash;
  size_t sym_count;
  SymMap* extra;
  int refs;
  bool initialized;
  Object** needed;
  size_t needed_count;
};

Object* g_objects;
Object* g_libc;
W32_SRWLOCK g_lock;
char g_error[512];
bool g_error_set;
char g_search_path[2][W32_PATH_MAX];
unsigned long long g_adds = 1, g_subs;

struct Lock {
  Lock() { w32.AcquireSRWLockExclusive(&g_lock); }
  ~Lock() { w32.ReleaseSRWLockExclusive(&g_lock); }
};

void set_error(const char* fmt, ...) __printflike(1, 2);
void set_error(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(g_error, sizeof g_error, fmt, ap);
  va_end(ap);
  g_error_set = true;
}

uint32_t sysv_hash_of(const char* s) {
  uint32_t h = 0;
  for (; *s; s++) {
    h = (h << 4) + static_cast<uint8_t>(*s);
    uint32_t g = h & 0xf0000000;
    h ^= g >> 24;
    h &= ~g;
  }
  return h;
}

bool defined(const ElfW(Sym)* s) {
  return s->st_shndx != SHN_UNDEF && (ELF64_ST_BIND(s->st_info) == STB_GLOBAL || ELF64_ST_BIND(s->st_info) == STB_WEAK ||
                                      ELF64_ST_BIND(s->st_info) == STB_GNU_UNIQUE) &&
         (s->st_other & 3) != STV_HIDDEN && ELF64_ST_TYPE(s->st_info) != STT_TLS;
}

const ElfW(Sym)* lookup_elf(const Object* o, const char* name, uint32_t gh) {
  if (o->gnu_hash) {
    const uint32_t* h = o->gnu_hash;
    uint32_t nbuckets = h[0], symoffset = h[1], bloom_size = h[2], bloom_shift = h[3];
    const uint64_t* bloom = reinterpret_cast<const uint64_t*>(h + 4);
    uint64_t word = bloom[(gh / 64) % bloom_size];
    if (!((word >> (gh % 64)) & (word >> ((gh >> bloom_shift) % 64)) & 1)) return nullptr;
    const uint32_t* buckets = reinterpret_cast<const uint32_t*>(bloom + bloom_size);
    const uint32_t* chain = buckets + nbuckets;
    uint32_t i = buckets[gh % nbuckets];
    if (i < symoffset) return nullptr;
    for (;; i++) {
      uint32_t ch = chain[i - symoffset];
      if ((ch | 1) == (gh | 1)) {
        const ElfW(Sym)* s = &o->symtab[i];
        if (strcmp(o->strtab + s->st_name, name) == 0 && defined(s)) return s;
      }
      if (ch & 1) return nullptr;
    }
  }
  if (o->sysv_hash) {
    uint32_t nbucket = o->sysv_hash[0];
    const uint32_t* bucket = o->sysv_hash + 2;
    const uint32_t* chain = bucket + nbucket;
    for (uint32_t i = bucket[sysv_hash_of(name) % nbucket]; i; i = chain[i]) {
      const ElfW(Sym)* s = &o->symtab[i];
      if (strcmp(o->strtab + s->st_name, name) == 0 && defined(s)) return s;
    }
  }
  return nullptr;
}

void* lookup_in(const Object* o, const char* name, uint32_t gh, bool* weak) {
  *weak = false;
  if (void* p = map_find(o->extra, name, gh)) return p;
  if (!o->symtab) return nullptr;
  const ElfW(Sym)* s = lookup_elf(o, name, gh);
  if (!s) return nullptr;
  *weak = ELF64_ST_BIND(s->st_info) == STB_WEAK;
  return reinterpret_cast<void*>(o->base + s->st_value);
}

void* lookup_scope(const char* name, int ns, const Object* after = nullptr) {
  uint32_t gh = gnu_hash_of(name);
  void* weak_hit = nullptr;
  bool searching = after == nullptr;
  for (int pass = 0; pass < 2; pass++) {
    for (const Object* o = g_objects; o; o = o->next) {
      bool libc = o == g_libc;
      if ((pass == 0) == libc || (!libc && o->ns != ns)) continue;
      if (!searching) {
        if (o == after) searching = true;
        continue;
      }
      bool weak;
      void* p = lookup_in(o, name, gh, &weak);
      if (!p) continue;
      if (!weak) return p;
      if (!weak_hit) weak_hit = p;
    }
    if (pass == 0) searching = true;
  }
  return weak_hit;
}

bool parse_dynamic(Object* o) {
  for (const ElfW(Dyn)* d = o->dynamic; d->d_tag != DT_NULL; d++) {
    uintptr_t v = o->base + d->d_un.d_ptr;
    switch (d->d_tag) {
      case DT_STRTAB: o->strtab = reinterpret_cast<const char*>(v); break;
      case DT_SYMTAB: o->symtab = reinterpret_cast<const ElfW(Sym)*>(v); break;
      case DT_GNU_HASH: o->gnu_hash = reinterpret_cast<const uint32_t*>(v); break;
      case DT_HASH: o->sysv_hash = reinterpret_cast<const uint32_t*>(v); break;
      default: break;
    }
  }
  if (!o->strtab || !o->symtab || (!o->gnu_hash && !o->sysv_hash)) return false;
  if (o->sysv_hash) {
    o->sym_count = o->sysv_hash[1];
  } else {
    const uint32_t* h = o->gnu_hash;
    uint32_t nbuckets = h[0], symoffset = h[1], bloom_size = h[2];
    const uint32_t* buckets = reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint64_t*>(h + 4) + bloom_size);
    const uint32_t* chain = buckets + nbuckets;
    uint32_t last = 0;
    for (uint32_t b = 0; b < nbuckets; b++)
      if (buckets[b] > last) last = buckets[b];
    if (last >= symoffset) {
      while (!(chain[last - symoffset] & 1)) last++;
      o->sym_count = last + 1;
    } else {
      o->sym_count = symoffset;
    }
  }
  return true;
}

const char* soname_of(const Object* o) {
  if (!o->strtab || !o->dynamic) return nullptr;
  for (const ElfW(Dyn)* d = o->dynamic; d->d_tag != DT_NULL; d++)
    if (d->d_tag == DT_SONAME) return o->strtab + d->d_un.d_val;
  return nullptr;
}

bool is_libc_name(const char* base) {
  return strcmp(base, "libc.so") == 0 || strcmp(base, "libm.so") == 0 || strcmp(base, "libdl.so") == 0;
}

Object* find_loaded(const char* name, int ns) {
  const char* base = strrchr(name, '/');
  base = base ? base + 1 : name;
  for (Object* o = g_objects; o; o = o->next) {
    if (o == g_libc || o->ns != ns) continue;
    if (strcmp(o->name, base) == 0 || (o->path && strcmp(o->path, name) == 0)) return o;
    const char* so = soname_of(o);
    if (so && strcmp(so, base) == 0) return o;
  }
  if (is_libc_name(base)) return g_libc;
  return nullptr;
}

long read_full(int fd, void* buf, size_t len, off_t off) {
  size_t done = 0;
  while (done < len) {
    ssize_t n = pread64(fd, static_cast<char*>(buf) + done, len - done, off + static_cast<off_t>(done));
    if (n <= 0) return n < 0 ? -errno : -EIO;
    done += static_cast<size_t>(n);
  }
  return 0;
}

DWORD protection(uint32_t flags) {
  bool x = flags & PF_X, w = flags & PF_W;
  if (x) return w ? 0x40 : 0x20;
  return w ? 0x04 : 0x02;
}

Object* map_object(const char* path, const char* name, int ns) {
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    set_error("cannot open \"%s\": %s", path, strerror(errno));
    return nullptr;
  }
  ElfW(Ehdr) eh;
  if (read_full(fd, &eh, sizeof eh, 0) < 0 || memcmp(eh.e_ident, ELFMAG, SELFMAG) || eh.e_ident[EI_CLASS] != ELFCLASS64 ||
      eh.e_machine != EM_X86_64 || eh.e_type != ET_DYN || eh.e_phentsize != sizeof(ElfW(Phdr))) {
    set_error("\"%s\" is not an x86_64 Android shared object", path);
    close(fd);
    return nullptr;
  }
  size_t ph_size = eh.e_phnum * sizeof(ElfW(Phdr));
  auto* ph = static_cast<ElfW(Phdr)*>(malloc(ph_size));
  if (!ph || read_full(fd, ph, ph_size, static_cast<off_t>(eh.e_phoff)) < 0) {
    free(ph);
    close(fd);
    set_error("\"%s\": cannot read program headers", path);
    return nullptr;
  }
  uintptr_t lo = UINTPTR_MAX, hi = 0;
  for (size_t i = 0; i < eh.e_phnum; i++) {
    if (ph[i].p_type == PT_LOAD) {
      if ((ph[i].p_vaddr & ~(kPage - 1)) < lo) lo = ph[i].p_vaddr & ~(kPage - 1);
      if (ph[i].p_vaddr + ph[i].p_memsz > hi) hi = ph[i].p_vaddr + ph[i].p_memsz;
    }
    if (ph[i].p_type == PT_TLS) {
      set_error("\"%s\" uses ELF TLS, which this loader does not support (build it for API < 29)", path);
      free(ph);
      close(fd);
      return nullptr;
    }
  }
  size_t span = ((hi - lo) + kPage - 1) & ~(kPage - 1);
  auto* map = static_cast<uint8_t*>(w32.VirtualAlloc(nullptr, span, 0x3000, 0x04));
  if (!map) {
    set_error("\"%s\": out of address space", path);
    free(ph);
    close(fd);
    return nullptr;
  }
  uintptr_t base = reinterpret_cast<uintptr_t>(map) - lo;
  Object* o = static_cast<Object*>(calloc(1, sizeof(Object)));
  o->ns = ns;
  o->base = base;
  o->map_start = reinterpret_cast<uintptr_t>(map);
  o->map_size = span;
  o->phnum = eh.e_phnum;
  for (size_t i = 0; i < eh.e_phnum; i++) {
    if (ph[i].p_type == PT_LOAD && ph[i].p_filesz &&
        read_full(fd, reinterpret_cast<void*>(base + ph[i].p_vaddr), ph[i].p_filesz, static_cast<off_t>(ph[i].p_offset)) < 0) {
      set_error("\"%s\": cannot read a segment", path);
      close(fd);
      w32.VirtualFree(map, 0, 0x8000);
      free(ph);
      free(o);
      return nullptr;
    }
    if (ph[i].p_type == PT_DYNAMIC) o->dynamic = reinterpret_cast<const ElfW(Dyn)*>(base + ph[i].p_vaddr);
  }
  close(fd);
  o->phdr = ph;
  for (size_t i = 0; i < eh.e_phnum; i++)
    if (ph[i].p_type == PT_PHDR) {
      o->phdr = reinterpret_cast<const ElfW(Phdr)*>(base + ph[i].p_vaddr);
      free(ph);
      break;
    }
  if (!o->dynamic || !parse_dynamic(o)) {
    set_error("\"%s\" has no usable dynamic section", path);
    w32.VirtualFree(map, 0, 0x8000);
    free(o);
    return nullptr;
  }
  o->name = strdup(name);
  o->path = strdup(path);
  const size_t len = strlen(path);
  if (len >= 13 && strcmp(path + len - 13, "/libroblox.so") == 0) {
    for (size_t i = 0; i < o->phnum; i++) {
      const ElfW(Phdr)& seg = o->phdr[i];
      if (seg.p_type != PT_LOAD || !(seg.p_flags & PF_X)) continue;
      w32_vex_set_roblox_range(base + seg.p_vaddr, base + seg.p_vaddr + seg.p_memsz);
      break;
    }
  }
  return o;
}

bool relocate_one(Object* o, uint64_t offset, uint64_t info, int64_t addend) {
  uint32_t type = ELF64_R_TYPE(info);
  uint32_t sym = ELF64_R_SYM(info);
  auto* where = reinterpret_cast<uintptr_t*>(o->base + offset);
  if (type == R_X86_64_RELATIVE) {
    *where = o->base + addend;
    return true;
  }
  uintptr_t value = 0;
  if (sym) {
    const ElfW(Sym)* s = &o->symtab[sym];
    const char* name = o->strtab + s->st_name;
    if (ELF64_ST_BIND(s->st_info) == STB_LOCAL) {
      value = o->base + s->st_value;
    } else {
      void* addr = lookup_scope(name, o->ns);
      if (!addr && s->st_shndx != SHN_UNDEF && defined(s)) addr = reinterpret_cast<void*>(o->base + s->st_value);
      if (!addr && ELF64_ST_BIND(s->st_info) != STB_WEAK) {
        set_error("cannot locate symbol \"%s\" referenced by \"%s\"", name, o->name);
        return false;
      }
      value = reinterpret_cast<uintptr_t>(addr);
    }
  }
  switch (type) {
    case R_X86_64_NONE: break;
    case R_X86_64_64: *where = value + addend; break;
    case R_X86_64_GLOB_DAT:
    case R_X86_64_JUMP_SLOT: *where = value; break;
    case R_X86_64_PC32: *reinterpret_cast<uint32_t*>(where) = static_cast<uint32_t>(value + addend - reinterpret_cast<uintptr_t>(where)); break;
    case R_X86_64_IRELATIVE: *where = reinterpret_cast<uintptr_t (*)()>(o->base + addend)(); break;
    default:
      set_error("unsupported relocation type %u in \"%s\"", type, o->name);
      return false;
  }
  return true;
}

bool apply_rela(Object* o, const ElfW(Rela)* rela, size_t count) {
  for (size_t i = 0; i < count; i++)
    if (!relocate_one(o, rela[i].r_offset, rela[i].r_info, rela[i].r_addend)) return false;
  return true;
}

bool apply_android_packed(Object* o, const uint8_t* data, size_t size, bool has_addends_section) {
  if (size < 4 || memcmp(data, "APS2", 4) != 0) {
    set_error("\"%s\": unknown packed relocation format", o->name);
    return false;
  }
  const uint8_t* p = data + 4;
  const uint8_t* end = data + size;
  bool bad = false;
  auto next = [&]() -> int64_t {
    int64_t value = 0;
    unsigned shift = 0;
    uint8_t byte;
    do {
      if (p >= end) {
        bad = true;
        return 0;
      }
      byte = *p++;
      value |= static_cast<int64_t>(byte & 0x7f) << shift;
      shift += 7;
    } while (byte & 0x80);
    if (shift < 64 && (byte & 0x40)) value |= -(static_cast<int64_t>(1) << shift);
    return value;
  };
  enum { kByInfo = 1, kByOffsetDelta = 2, kByAddend = 4, kHasAddend = 8 };
  int64_t count = next();
  uint64_t offset = static_cast<uint64_t>(next());
  uint64_t info = 0;
  int64_t addend = 0;
  (void)has_addends_section;
  while (count > 0 && !bad) {
    int64_t group_size = next();
    int64_t flags = next();
    int64_t offset_delta = (flags & kByOffsetDelta) ? next() : 0;
    if (flags & kByInfo) info = static_cast<uint64_t>(next());
    if ((flags & kHasAddend) && (flags & kByAddend)) addend += next();
    else if (!(flags & kHasAddend)) addend = 0;
    for (int64_t i = 0; i < group_size && !bad; i++) {
      offset += (flags & kByOffsetDelta) ? static_cast<uint64_t>(offset_delta) : static_cast<uint64_t>(next());
      if (!(flags & kByInfo)) info = static_cast<uint64_t>(next());
      if ((flags & kHasAddend) && !(flags & kByAddend)) addend += next();
      if (!relocate_one(o, offset, info, addend)) return false;
    }
    count -= group_size;
  }
  if (bad) {
    set_error("\"%s\": truncated packed relocations", o->name);
    return false;
  }
  return true;
}

void apply_relr(Object* o, const ElfW(Relr)* relr, size_t size) {
  uintptr_t* where = nullptr;
  for (size_t i = 0; i < size / sizeof(ElfW(Relr)); i++) {
    ElfW(Relr) e = relr[i];
    if ((e & 1) == 0) {
      where = reinterpret_cast<uintptr_t*>(o->base + e);
      *where++ += o->base;
    } else {
      for (int bit = 0; (e >>= 1) != 0; bit++)
        if (e & 1) where[bit] += o->base;
      where += 63;
    }
  }
}

bool relocate(Object* o) {
  const ElfW(Rela)* rela = nullptr;
  const ElfW(Rela)* jmprel = nullptr;
  const ElfW(Relr)* relr = nullptr;
  const uint8_t* packed = nullptr;
  size_t relasz = 0, pltrelsz = 0, relrsz = 0, packedsz = 0;
  for (const ElfW(Dyn)* d = o->dynamic; d->d_tag != DT_NULL; d++) {
    uintptr_t v = o->base + d->d_un.d_ptr;
    switch (d->d_tag) {
      case DT_RELA: rela = reinterpret_cast<const ElfW(Rela)*>(v); break;
      case DT_RELASZ: relasz = d->d_un.d_val; break;
      case DT_JMPREL: jmprel = reinterpret_cast<const ElfW(Rela)*>(v); break;
      case DT_PLTRELSZ: pltrelsz = d->d_un.d_val; break;
      case DT_RELR:
      case 0x6fffe000:  // DT_ANDROID_RELR
        relr = reinterpret_cast<const ElfW(Relr)*>(v);
        break;
      case DT_RELRSZ:
      case 0x6fffe001:  // DT_ANDROID_RELRSZ
        relrsz = d->d_un.d_val;
        break;
      case DT_ANDROID_RELA: packed = reinterpret_cast<const uint8_t*>(v); break;
      case DT_ANDROID_RELASZ: packedsz = d->d_un.d_val; break;
      case DT_ANDROID_REL:
        set_error("\"%s\" uses packed REL relocations (32-bit only)", o->name);
        return false;
      default: break;
    }
  }
  if (relr) apply_relr(o, relr, relrsz);
  if (packed && !apply_android_packed(o, packed, packedsz, true)) return false;
  if (rela && !apply_rela(o, rela, relasz / sizeof(ElfW(Rela)))) return false;
  if (jmprel && !apply_rela(o, jmprel, pltrelsz / sizeof(ElfW(Rela)))) return false;
  return true;
}

void protect(Object* o) {
  w32_mem_forget_runs(o->map_start, o->map_start + o->map_size);
  for (size_t i = 0; i < o->phnum; i++) {
    const ElfW(Phdr)& p = o->phdr[i];
    if (p.p_type != PT_LOAD) continue;
    uintptr_t start = (o->base + p.p_vaddr) & ~(kPage - 1);
    uintptr_t end = (o->base + p.p_vaddr + p.p_memsz + kPage - 1) & ~(kPage - 1);
    DWORD old;
    if (w32.VirtualProtect(reinterpret_cast<void*>(start), end - start, protection(p.p_flags), &old)) {
      w32_mem_note_run(start, end, o->map_start, protection(p.p_flags));
    } else {
      w32_mem_forget_runs(start, end);
    }
  }
  for (size_t i = 0; i < o->phnum; i++) {
    const ElfW(Phdr)& p = o->phdr[i];
    if (p.p_type != PT_GNU_RELRO) continue;
    uintptr_t start = (o->base + p.p_vaddr) & ~(kPage - 1);
    uintptr_t end = (o->base + p.p_vaddr + p.p_memsz) & ~(kPage - 1);
    DWORD old;
    if (end > start) {
      if (w32.VirtualProtect(reinterpret_cast<void*>(start), end - start, 0x02, &old)) {
        w32_mem_note_run(start, end, o->map_start, 0x02);
      } else {
        w32_mem_forget_runs(start, end);
      }
    }
  }
  w32.FlushInstructionCache(w32.GetCurrentProcess(), reinterpret_cast<void*>(o->map_start), o->map_size);
}

bool find_file(const char* name, const char* requester_dir, int ns, char* out) {
  if (strchr(name, '/')) {
    snprintf(out, W32_PATH_MAX, "%s", name);
    return access(out, F_OK) == 0;
  }
  char paths[W32_PATH_MAX * 2];
  snprintf(paths, sizeof paths, "%s:%s", requester_dir ? requester_dir : "", g_search_path[ns]);
  for (char* dir = paths; *dir;) {
    char* end = strchr(dir, ':');
    if (end) *end = 0;
    if (*dir) {
      snprintf(out, W32_PATH_MAX, "%s/%s", dir, name);
      if (access(out, F_OK) == 0) return true;
    }
    if (!end) break;
    dir = end + 1;
  }
  return false;
}

void append(Object* o) {
  Object** tail = &g_objects;
  while (*tail) tail = &(*tail)->next;
  *tail = o;
  g_adds++;
}

Object* load(const char* name, const char* requester_dir, int ns, Object*** fresh, size_t* fresh_count, size_t* fresh_cap) {
  if (Object* o = find_loaded(name, ns)) return o;
  char path[W32_PATH_MAX];
  if (!find_file(name, requester_dir, ns, path)) {
    set_error("library \"%s\" not found", name);
    return nullptr;
  }
  const char* base = strrchr(name, '/');
  Object* o = map_object(path, base ? base + 1 : name, ns);
  if (!o) return nullptr;
  append(o);
  char dir[W32_PATH_MAX];
  snprintf(dir, sizeof dir, "%s", path);
  if (char* slash = strrchr(dir, '/')) *slash = 0;
  size_t count = 0;
  for (const ElfW(Dyn)* d = o->dynamic; d->d_tag != DT_NULL; d++)
    if (d->d_tag == DT_NEEDED) count++;
  o->needed = static_cast<Object**>(calloc(count ? count : 1, sizeof(Object*)));
  for (const ElfW(Dyn)* d = o->dynamic; d->d_tag != DT_NULL; d++) {
    if (d->d_tag != DT_NEEDED) continue;
    Object* dep = load(o->strtab + d->d_un.d_val, dir, ns, fresh, fresh_count, fresh_cap);
    if (!dep) return nullptr;
    o->needed[o->needed_count++] = dep;
  }
  if (*fresh_count == *fresh_cap) {
    *fresh_cap = *fresh_cap ? *fresh_cap * 2 : 8;
    *fresh = static_cast<Object**>(realloc(*fresh, *fresh_cap * sizeof(Object*)));
  }
  (*fresh)[(*fresh_count)++] = o;
  return o;
}

void (*g_before_constructors)(const char*, uintptr_t);
bool (*g_skip_constructors)(const char*);

void run_constructors(Object* o, int argc, char** argv, char** envp) {
  if (!o->dynamic) return;
  if (o->ns == kGuest && o->path) {
    if (g_before_constructors) g_before_constructors(o->path, o->base);
    if (g_skip_constructors && g_skip_constructors(o->path)) return;
  }
  void (*init)(int, char**, char**) = nullptr;
  void (**init_array)(int, char**, char**) = nullptr;
  size_t init_count = 0;
  for (const ElfW(Dyn)* d = o->dynamic; d->d_tag != DT_NULL; d++) {
    uintptr_t v = o->base + d->d_un.d_ptr;
    switch (d->d_tag) {
      case DT_INIT: init = reinterpret_cast<void (*)(int, char**, char**)>(v); break;
      case DT_INIT_ARRAY: init_array = reinterpret_cast<void (**)(int, char**, char**)>(v); break;
      case DT_INIT_ARRAYSZ: init_count = d->d_un.d_val / sizeof(void*); break;
      default: break;
    }
  }
  if (init) init(argc, argv, envp);
  for (size_t i = 0; i < init_count; i++) {
    uintptr_t f = reinterpret_cast<uintptr_t>(init_array[i]);
    if (f != 0 && f != UINTPTR_MAX) init_array[i](argc, argv, envp);
  }
}

int g_argc;
char** g_argv;
char g_scope_handle[2];

void* open_library(const char* name, int ns) {
  Object** fresh = nullptr;
  size_t fresh_count = 0, fresh_cap = 0;
  Object* o;
  {
    Lock lock;
    o = load(name, nullptr, ns, &fresh, &fresh_count, &fresh_cap);
    bool ok = o != nullptr;
    for (size_t i = 0; ok && i < fresh_count; i++) {
      ok = relocate(fresh[i]);
      if (ok) protect(fresh[i]);
    }
    if (!ok) {
      for (Object** p = &g_objects; *p;) {
        bool mine = false;
        for (size_t i = 0; i < fresh_count; i++) mine |= *p == fresh[i];
        if (mine || (!(*p)->initialized && *p != g_libc && !(*p)->extra && (*p)->path)) {
          Object* dead = *p;
          *p = dead->next;
          w32.VirtualFree(reinterpret_cast<void*>(dead->map_start), 0, 0x8000);
          w32_mem_forget_runs(dead->map_start, dead->map_start + dead->map_size);
          g_subs++;
        } else {
          p = &(*p)->next;
        }
      }
      free(fresh);
      return nullptr;
    }
    o->refs++;
  }
  for (size_t i = 0; i < fresh_count; i++) {
    fresh[i]->initialized = true;
    run_constructors(fresh[i], g_argc, g_argv, environ);
  }
  free(fresh);
  return o;
}

void* dlsym_in(void* handle, const char* symbol, int ns, uintptr_t caller) {
  g_error_set = false;
  void* r = nullptr;
  {
    Lock lock;
    if (handle == RTLD_DEFAULT || handle == &g_scope_handle[0] || handle == &g_scope_handle[1]) {
      r = lookup_scope(symbol, handle == &g_scope_handle[1] ? kGuest : handle == &g_scope_handle[0] ? kHost : ns);
    } else if (handle == RTLD_NEXT) {
      const Object* after = nullptr;
      for (const Object* o = g_objects; o; o = o->next)
        if (caller >= o->map_start && caller < o->map_start + o->map_size) after = o;
      r = lookup_scope(symbol, ns, after ? after : g_libc);
    } else {
      Object* queue[256];
      size_t head = 0, tail = 0;
      queue[tail++] = static_cast<Object*>(handle);
      uint32_t gh = gnu_hash_of(symbol);
      while (head < tail && !r) {
        Object* o = queue[head++];
        bool weak;
        if ((r = lookup_in(o, symbol, gh, &weak))) break;
        for (size_t i = 0; i < o->needed_count && tail < 256; i++) {
          bool seen = false;
          for (size_t k = 0; k < tail; k++) seen |= queue[k] == o->needed[i];
          if (!seen) queue[tail++] = o->needed[i];
        }
      }
    }
  }
  if (!r) set_error("undefined symbol: %s", symbol);
  return r;
}

}  // namespace

char g_libc_path[W32_PATH_MAX];

uint64_t w32_loader_generation() { return __atomic_load_n(&g_adds, __ATOMIC_RELAXED) + __atomic_load_n(&g_subs, __ATOMIC_RELAXED); }

void w32_loader_segments(void (*fn)(void*, uintptr_t, uintptr_t, uintptr_t, uint64_t, const char*), void* ctx) {
  Lock lock;
  for (Object* o = g_objects; o; o = o->next) {
    const char* path = o == g_libc ? g_libc_path : o->path;
    if (!path || !path[0]) continue;
    for (size_t i = 0; i < o->phnum; i++) {
      const ElfW(Phdr)& ph = o->phdr[i];
      if (ph.p_type != PT_LOAD) continue;
      uintptr_t start = (o->base + ph.p_vaddr) & ~(kPage - 1);
      uintptr_t file_end = (o->base + ph.p_vaddr + ph.p_filesz + kPage - 1) & ~(kPage - 1);
      uintptr_t end = (o->base + ph.p_vaddr + ph.p_memsz + kPage - 1) & ~(kPage - 1);
      fn(ctx, start, file_end, end, ph.p_offset & ~static_cast<uint64_t>(kPage - 1), path);
    }
  }
}

void w32_init_loader() {
  Object* o = static_cast<Object*>(calloc(1, sizeof(Object)));
  o->name = strdup("libc.so");
  o->base = w32_boot->libc_base;
  o->phdr = static_cast<const ElfW(Phdr)*>(w32_boot->libc_phdr);
  o->phnum = w32_boot->libc_phnum;
  o->refs = 1;
  o->initialized = true;
  uintptr_t hi = 0;
  for (size_t i = 0; i < o->phnum; i++) {
    if (o->phdr[i].p_type == PT_DYNAMIC) o->dynamic = reinterpret_cast<const ElfW(Dyn)*>(o->base + o->phdr[i].p_vaddr);
    if (o->phdr[i].p_type == PT_LOAD && o->phdr[i].p_vaddr + o->phdr[i].p_memsz > hi) hi = o->phdr[i].p_vaddr + o->phdr[i].p_memsz;
  }
  o->map_start = o->base;
  o->map_size = hi;
  if (!o->dynamic || !parse_dynamic(o)) w32_fatal("libc.so has no symbol table");
  g_libc = o;
  g_objects = o;
  const char* lp = getenv("LD_LIBRARY_PATH");
  WCHAR exe_dir[W32_PATH_MAX];
  char runtime_dir[W32_PATH_MAX] = "";
  if (w32_utf8_to_utf16(w32_boot->exe_dir, exe_dir, W32_PATH_MAX) >= 0 &&
      w32_unix_path(exe_dir, runtime_dir, sizeof runtime_dir) >= 0) {
    size_t n = strlen(runtime_dir);
    if (n > 1 && runtime_dir[n - 1] == '/') runtime_dir[n - 1] = 0;
    snprintf(g_libc_path, sizeof g_libc_path, "%s/libc.so", runtime_dir);
  }
  snprintf(g_search_path[kHost], sizeof g_search_path[kHost], "%s%s%s", lp && *lp ? lp : "",
           lp && *lp && runtime_dir[0] ? ":" : "", runtime_dir);
}

void* w32_load_program(const char* path, char* error, size_t error_size) {
  g_argc = w32_boot->argc;
  g_argv = w32_boot->argv;
  char dir[W32_PATH_MAX];
  snprintf(dir, sizeof dir, "%s", path);
  if (char* slash = strrchr(dir, '/')) {
    *slash = 0;
    char sp[W32_PATH_MAX];
    snprintf(sp, sizeof sp, "%s%s%s", dir, g_search_path[kHost][0] ? ":" : "", g_search_path[kHost]);
    snprintf(g_search_path[kHost], sizeof g_search_path[kHost], "%s", sp);
  }
  void* handle = open_library(path, kHost);
  void* main_fn = handle ? dlsym_in(handle, "main", kHost, 0) : nullptr;
  if (!main_fn) snprintf(error, error_size, "%s", g_error_set ? g_error : "no main()");
  return main_fn;
}

extern "C" {

void* dlopen(const char* name, int flags) {
  (void)flags;
  if (!name) return &g_scope_handle[kHost];
  g_error_set = false;
  return open_library(name, kHost);
}

void* android_dlopen_ext(const char* name, int flags, const void*) { return dlopen(name, flags); }

void* dlsym(void* handle, const char* symbol) {
  return dlsym_in(handle, symbol, kHost, reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
}

void* dlvsym(void* handle, const char* symbol, const char*) {
  return dlsym_in(handle, symbol, kHost, reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
}

int dlclose(void* handle) {
  // never unmap, android code loves pointers into 'closed' libraries
  Lock lock;
  if (handle == &g_scope_handle[0] || handle == &g_scope_handle[1]) return 0;
  Object* o = static_cast<Object*>(handle);
  if (o && o->refs > 0) o->refs--;
  return 0;
}

char* dlerror() {
  if (!g_error_set) return nullptr;
  g_error_set = false;
  return g_error;
}

int dladdr(const void* addr, Dl_info* info) {
  uintptr_t a = reinterpret_cast<uintptr_t>(addr);
  Lock lock;
  for (const Object* o = g_objects; o; o = o->next) {
    if (!o->map_size || a < o->map_start || a >= o->map_start + o->map_size) continue;
    info->dli_fname = o->path ? o->path : "/system/lib64/libc.so";
    info->dli_fbase = reinterpret_cast<void*>(o->map_start);
    info->dli_sname = nullptr;
    info->dli_saddr = nullptr;
    uintptr_t best = 0;
    for (size_t i = 1; i < o->sym_count; i++) {
      const ElfW(Sym)* s = &o->symtab[i];
      if (s->st_shndx == SHN_UNDEF || ELF64_ST_TYPE(s->st_info) == STT_TLS) continue;
      uintptr_t start = o->base + s->st_value;
      if (start <= a && start > best && (a < start + s->st_size || s->st_size == 0)) {
        best = start;
        info->dli_sname = o->strtab + s->st_name;
        info->dli_saddr = reinterpret_cast<void*>(start);
      }
    }
    return 1;
  }
  return 0;
}

int dl_iterate_phdr(int (*cb)(dl_phdr_info*, size_t, void*), void* data) {
  dl_phdr_info infos[512];
  size_t n = 0;
  {
    Lock lock;
    for (const Object* o = g_objects; o && n < 512; o = o->next) {
      if (!o->phdr) continue;
      dl_phdr_info& i = infos[n++];
      i.dlpi_addr = o->base;
      i.dlpi_name = o->path ? o->path : "/system/lib64/libc.so";
      i.dlpi_phdr = o->phdr;
      i.dlpi_phnum = o->phnum;
      i.dlpi_adds = g_adds;
      i.dlpi_subs = g_subs;
      i.dlpi_tls_modid = 0;
      i.dlpi_tls_data = nullptr;
    }
  }
  for (size_t i = 0; i < n; i++)
    if (int r = cb(&infos[i], sizeof(dl_phdr_info), data)) return r;
  return 0;
}

void android_get_LD_LIBRARY_PATH(char* buffer, size_t size) { snprintf(buffer, size, "%s", g_search_path[kHost]); }
void android_update_LD_LIBRARY_PATH(const char* path) {
  snprintf(g_search_path[kHost], sizeof g_search_path[kHost], "%s", path);
}

#define W32_EXPORT __attribute__((visibility("default")))

W32_EXPORT void* __w32_ns_dlopen(const char* name, int flags) {
  (void)flags;
  if (!name) return &g_scope_handle[kGuest];
  g_error_set = false;
  return open_library(name, kGuest);
}

W32_EXPORT void* __w32_ns_dlsym(void* handle, const char* symbol, const void* caller) {
  return dlsym_in(handle, symbol, kGuest, reinterpret_cast<uintptr_t>(caller));
}

W32_EXPORT void __w32_ns_set_library_path(const char* path) {
  snprintf(g_search_path[kGuest], sizeof g_search_path[kGuest], "%s", path);
}

W32_EXPORT void* __w32_ns_synthetic(const char* soname) {
  Lock lock;
  for (Object* o = g_objects; o; o = o->next)
    if (o->ns == kGuest && !o->path && strcmp(o->name, soname) == 0) return o;
  Object* o = static_cast<Object*>(calloc(1, sizeof(Object)));
  o->ns = kGuest;
  o->name = strdup(soname);
  o->extra = static_cast<SymMap*>(calloc(1, sizeof(SymMap)));
  o->refs = 1;
  o->initialized = true;
  append(o);
  return o;
}

W32_EXPORT void __w32_ns_add_symbols(void* handle, const char* const* names, void* const* addrs, size_t count) {
  Lock lock;
  Object* o = static_cast<Object*>(handle);
  if (!o->extra) o->extra = static_cast<SymMap*>(calloc(1, sizeof(SymMap)));
  for (size_t i = 0; i < count; i++) map_put(o->extra, names[i], addrs[i]);
}

W32_EXPORT uintptr_t __w32_ns_base(void* handle) { return static_cast<Object*>(handle)->base; }

W32_EXPORT void __w32_ns_code_region(void* handle, uintptr_t* base, size_t* size) {
  Object* o = static_cast<Object*>(handle);
  *base = 0;
  *size = 0;
  for (size_t i = 0; i < o->phnum; i++) {
    if (o->phdr[i].p_type == PT_LOAD && (o->phdr[i].p_flags & PF_X)) {
      *base = o->base + o->phdr[i].p_vaddr;
      *size = o->phdr[i].p_memsz;
    }
  }
}

W32_EXPORT int __w32_ns_refs(void* handle) { return static_cast<Object*>(handle)->refs; }

W32_EXPORT void __w32_ns_set_constructor_hooks(void (*before)(const char*, uintptr_t), bool (*skip)(const char*)) {
  g_before_constructors = before;
  g_skip_constructors = skip;
}

}  // extern "C"

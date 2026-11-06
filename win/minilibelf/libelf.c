#include "gelf.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct Elf_Scn {
  Elf* elf;
  size_t index;
  Elf_Data data;
  int data_given;
};

struct Elf {
  char* image;
  size_t size;
  int owns_image;
  Elf_Kind kind;
  const Elf64_Ehdr* ehdr;
  size_t shnum, shstrndx, phnum;
  Elf_Scn* scns;
};

static __thread int g_error;

enum { E_NONE, E_NOMEM, E_IO, E_FORMAT, E_RANGE, E_NOT64 };

static void* fail(int e) {
  g_error = e;
  return NULL;
}

unsigned int elf_version(unsigned int version) { return version == EV_NONE ? EV_CURRENT : EV_CURRENT; }

static Elf* open_image(char* image, size_t size, int owns) {
  Elf* e = calloc(1, sizeof(Elf));
  if (!e) return fail(E_NOMEM);
  e->image = image;
  e->size = size;
  e->owns_image = owns;
  e->kind = ELF_K_NONE;
  if (size >= sizeof(Elf64_Ehdr) && memcmp(image, ELFMAG, SELFMAG) == 0) {
    e->kind = ELF_K_ELF;
    if (image[EI_CLASS] == ELFCLASS64 && image[EI_DATA] == ELFDATA2LSB) {
      e->ehdr = (const Elf64_Ehdr*)image;
      const Elf64_Ehdr* h = e->ehdr;
      e->shnum = h->e_shnum;
      e->shstrndx = h->e_shstrndx;
      e->phnum = h->e_phnum;
      if (h->e_shoff && h->e_shoff + sizeof(Elf64_Shdr) <= size) {
        const Elf64_Shdr* sh0 = (const Elf64_Shdr*)(image + h->e_shoff);
        if (e->shnum == 0) e->shnum = sh0->sh_size;
        if (e->shstrndx == SHN_XINDEX) e->shstrndx = sh0->sh_link;
      }
      if (h->e_shoff + e->shnum * sizeof(Elf64_Shdr) > size) e->shnum = 0;
      if (h->e_phoff + e->phnum * sizeof(Elf64_Phdr) > size) e->phnum = 0;
      e->scns = calloc(e->shnum ? e->shnum : 1, sizeof(Elf_Scn));
      if (!e->scns) {
        free(e);
        return fail(E_NOMEM);
      }
      for (size_t i = 0; i < e->shnum; i++) {
        e->scns[i].elf = e;
        e->scns[i].index = i;
      }
    }
  } else if (size >= 8 && memcmp(image, "!<arch>\n", 8) == 0) {
    e->kind = ELF_K_AR;
  }
  return e;
}

Elf* elf_begin(int fd, Elf_Cmd cmd, Elf* ref) {
  (void)ref;
  if (cmd == ELF_C_NULL) return NULL;
  if (cmd != ELF_C_READ && cmd != ELF_C_READ_MMAP && cmd != ELF_C_READ_MMAP_PRIVATE && cmd != ELF_C_RDWR) return fail(E_RANGE);
  struct stat st;
  if (fstat(fd, &st) != 0 || st.st_size < 0) return fail(E_IO);
  size_t size = (size_t)st.st_size;
  char* image = malloc(size ? size : 1);
  if (!image) return fail(E_NOMEM);
  for (size_t done = 0; done < size;) {
    ssize_t n = pread(fd, image + done, size - done, (off_t)done);
    if (n <= 0) {
      free(image);
      return fail(E_IO);
    }
    done += (size_t)n;
  }
  Elf* e = open_image(image, size, 1);
  if (!e) free(image);
  return e;
}

Elf* elf_memory(char* image, size_t size) { return open_image(image, size, 0); }

int elf_end(Elf* e) {
  if (!e) return 0;
  if (e->owns_image) free(e->image);
  free(e->scns);
  free(e);
  return 0;
}

Elf_Kind elf_kind(Elf* e) { return e ? e->kind : ELF_K_NONE; }

int gelf_getclass(Elf* e) { return e && e->kind == ELF_K_ELF ? e->image[EI_CLASS] : ELFCLASSNONE; }

GElf_Ehdr* gelf_getehdr(Elf* e, GElf_Ehdr* out) {
  if (!e || !e->ehdr) return fail(E_NOT64);
  *out = *e->ehdr;
  return out;
}

static const Elf64_Shdr* shdr(Elf* e, size_t i) { return (const Elf64_Shdr*)(e->image + e->ehdr->e_shoff) + i; }

Elf_Scn* elf_getscn(Elf* e, size_t index) {
  if (!e || !e->ehdr || index >= e->shnum) return fail(E_RANGE);
  return &e->scns[index];
}

Elf_Scn* elf_nextscn(Elf* e, Elf_Scn* scn) {
  size_t next = scn ? scn->index + 1 : 1;
  return e && e->ehdr && next < e->shnum ? &e->scns[next] : NULL;
}

size_t elf_ndxscn(Elf_Scn* scn) { return scn ? scn->index : SHN_UNDEF; }

GElf_Shdr* gelf_getshdr(Elf_Scn* scn, GElf_Shdr* out) {
  if (!scn) return fail(E_RANGE);
  *out = *shdr(scn->elf, scn->index);
  return out;
}

static Elf_Type type_of(uint32_t sh_type) {
  switch (sh_type) {
    case SHT_SYMTAB: case SHT_DYNSYM: return ELF_T_SYM;
    case SHT_DYNAMIC: return ELF_T_DYN;
    case SHT_RELA: return ELF_T_RELA;
    case SHT_REL: return ELF_T_REL;
    case SHT_NOTE: return ELF_T_NHDR;
    default: return ELF_T_BYTE;
  }
}

Elf_Data* elf_getdata(Elf_Scn* scn, Elf_Data* data) {
  if (!scn || data) return NULL;
  const Elf64_Shdr* s = shdr(scn->elf, scn->index);
  Elf_Data* d = &scn->data;
  d->d_type = type_of(s->sh_type);
  d->d_version = EV_CURRENT;
  d->d_off = 0;
  d->d_align = s->sh_addralign;
  if (s->sh_type == SHT_NOBITS) {
    d->d_buf = NULL;
    d->d_size = s->sh_size;
  } else {
    if (s->sh_offset > scn->elf->size || s->sh_size > scn->elf->size - s->sh_offset) return fail(E_FORMAT);
    d->d_buf = scn->elf->image + s->sh_offset;
    d->d_size = s->sh_size;
  }
  return d;
}

Elf_Data* elf_rawdata(Elf_Scn* scn, Elf_Data* data) { return elf_getdata(scn, data); }

char* elf_strptr(Elf* e, size_t section, size_t offset) {
  if (!e || !e->ehdr || section >= e->shnum) return fail(E_RANGE);
  const Elf64_Shdr* s = shdr(e, section);
  if (s->sh_type != SHT_STRTAB || offset >= s->sh_size || s->sh_offset + s->sh_size > e->size) return fail(E_RANGE);
  char* p = e->image + s->sh_offset + offset;
  if (!memchr(p, 0, s->sh_size - offset)) return fail(E_FORMAT);
  return p;
}

int elf_getshdrnum(Elf* e, size_t* count) {
  if (!e || !e->ehdr) return -1;
  *count = e->shnum;
  return 0;
}

int elf_getshdrstrndx(Elf* e, size_t* index) {
  if (!e || !e->ehdr) return -1;
  *index = e->shstrndx;
  return 0;
}

int elf_getphdrnum(Elf* e, size_t* count) {
  if (!e || !e->ehdr) return -1;
  *count = e->phnum;
  return 0;
}

GElf_Phdr* gelf_getphdr(Elf* e, int index, GElf_Phdr* out) {
  if (!e || !e->ehdr || index < 0 || (size_t)index >= e->phnum) return fail(E_RANGE);
  *out = ((const Elf64_Phdr*)(e->image + e->ehdr->e_phoff))[index];
  return out;
}

GElf_Sym* gelf_getsym(Elf_Data* d, int index, GElf_Sym* out) {
  if (!d || index < 0 || (size_t)(index + 1) * sizeof(Elf64_Sym) > d->d_size) return fail(E_RANGE);
  memcpy(out, (const char*)d->d_buf + (size_t)index * sizeof(Elf64_Sym), sizeof(Elf64_Sym));
  return out;
}

GElf_Dyn* gelf_getdyn(Elf_Data* d, int index, GElf_Dyn* out) {
  if (!d || index < 0 || (size_t)(index + 1) * sizeof(Elf64_Dyn) > d->d_size) return fail(E_RANGE);
  memcpy(out, (const char*)d->d_buf + (size_t)index * sizeof(Elf64_Dyn), sizeof(Elf64_Dyn));
  return out;
}

size_t gelf_getnote(Elf_Data* d, size_t offset, GElf_Nhdr* note, size_t* name_offset, size_t* desc_offset) {
  if (!d || offset + sizeof(Elf64_Nhdr) > d->d_size) return 0;
  memcpy(note, (const char*)d->d_buf + offset, sizeof(Elf64_Nhdr));
  size_t name = offset + sizeof(Elf64_Nhdr);
  size_t desc = name + ((note->n_namesz + 3) & ~(size_t)3);
  size_t next = desc + ((note->n_descsz + 3) & ~(size_t)3);
  if (next > d->d_size) return 0;
  *name_offset = name;
  *desc_offset = desc;
  return next;
}

char* elf_rawfile(Elf* e, size_t* size) {
  if (!e) return NULL;
  if (size) *size = e->size;
  return e->image;
}

int elf_errno(void) {
  int e = g_error;
  g_error = 0;
  return e;
}

const char* elf_errmsg(int error) {
  static const char* const kMessages[] = {"no error", "out of memory", "I/O error", "invalid ELF data", "out of range",
                                          "not a 64-bit ELF file"};
  if (error == -1) error = g_error;
  return error >= 0 && error < (int)(sizeof kMessages / sizeof kMessages[0]) ? kMessages[error] : "unknown error";
}

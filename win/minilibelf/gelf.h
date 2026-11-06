#ifndef BLOX7_WIN_GELF_H_
#define BLOX7_WIN_GELF_H_

#include "libelf.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef Elf64_Ehdr GElf_Ehdr;
typedef Elf64_Shdr GElf_Shdr;
typedef Elf64_Phdr GElf_Phdr;
typedef Elf64_Sym GElf_Sym;
typedef Elf64_Nhdr GElf_Nhdr;
typedef Elf64_Dyn GElf_Dyn;
typedef Elf64_Rela GElf_Rela;
typedef Elf64_Rel GElf_Rel;
typedef Elf64_Addr GElf_Addr;
typedef Elf64_Off GElf_Off;
typedef Elf64_Xword GElf_Xword;
typedef Elf64_Word GElf_Word;
typedef Elf64_Half GElf_Half;

#define GELF_ST_BIND ELF64_ST_BIND
#define GELF_ST_TYPE ELF64_ST_TYPE
#define GELF_ST_VISIBILITY(o) ((o) & 3)
#define GELF_R_SYM ELF64_R_SYM
#define GELF_R_TYPE ELF64_R_TYPE

int gelf_getclass(Elf* elf);
GElf_Ehdr* gelf_getehdr(Elf* elf, GElf_Ehdr* out);
GElf_Shdr* gelf_getshdr(Elf_Scn* scn, GElf_Shdr* out);
GElf_Phdr* gelf_getphdr(Elf* elf, int index, GElf_Phdr* out);
GElf_Sym* gelf_getsym(Elf_Data* data, int index, GElf_Sym* out);
GElf_Dyn* gelf_getdyn(Elf_Data* data, int index, GElf_Dyn* out);
size_t gelf_getnote(Elf_Data* data, size_t offset, GElf_Nhdr* note, size_t* name_offset, size_t* desc_offset);

#ifdef __cplusplus
}
#endif

#endif

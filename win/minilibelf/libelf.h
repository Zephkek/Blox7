#ifndef BLOX7_WIN_LIBELF_H_
#define BLOX7_WIN_LIBELF_H_

#include <elf.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { ELF_K_NONE, ELF_K_AR, ELF_K_COFF, ELF_K_ELF, ELF_K_NUM } Elf_Kind;
typedef enum {
  ELF_C_NULL, ELF_C_READ, ELF_C_RDWR, ELF_C_WRITE, ELF_C_CLR, ELF_C_SET, ELF_C_FDDONE, ELF_C_FDREAD,
  ELF_C_READ_MMAP, ELF_C_RDWR_MMAP, ELF_C_WRITE_MMAP, ELF_C_READ_MMAP_PRIVATE, ELF_C_EMPTY, ELF_C_NUM
} Elf_Cmd;
typedef enum { ELF_T_BYTE, ELF_T_ADDR, ELF_T_DYN, ELF_T_EHDR, ELF_T_HALF, ELF_T_OFF, ELF_T_PHDR, ELF_T_RELA,
               ELF_T_REL, ELF_T_SHDR, ELF_T_SWORD, ELF_T_SYM, ELF_T_WORD, ELF_T_XWORD, ELF_T_SXWORD,
               ELF_T_VDEF, ELF_T_VDAUX, ELF_T_VNEED, ELF_T_VNAUX, ELF_T_NHDR, ELF_T_NUM } Elf_Type;

typedef struct Elf Elf;
typedef struct Elf_Scn Elf_Scn;

typedef struct {
  void* d_buf;
  Elf_Type d_type;
  unsigned int d_version;
  size_t d_size;
  int64_t d_off;
  size_t d_align;
} Elf_Data;

unsigned int elf_version(unsigned int version);
Elf* elf_begin(int fd, Elf_Cmd cmd, Elf* ref);
Elf* elf_memory(char* image, size_t size);
int elf_end(Elf* elf);
Elf_Kind elf_kind(Elf* elf);
Elf_Scn* elf_getscn(Elf* elf, size_t index);
Elf_Scn* elf_nextscn(Elf* elf, Elf_Scn* scn);
size_t elf_ndxscn(Elf_Scn* scn);
Elf_Data* elf_getdata(Elf_Scn* scn, Elf_Data* data);
Elf_Data* elf_rawdata(Elf_Scn* scn, Elf_Data* data);
char* elf_strptr(Elf* elf, size_t section, size_t offset);
int elf_getshdrnum(Elf* elf, size_t* count);
int elf_getshdrstrndx(Elf* elf, size_t* index);
int elf_getphdrnum(Elf* elf, size_t* count);
char* elf_rawfile(Elf* elf, size_t* size);
int elf_errno(void);
const char* elf_errmsg(int error);

#ifdef __cplusplus
}
#endif

#endif

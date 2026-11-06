#ifndef BLOX7_WIN_W32BOOT_H_
#define BLOX7_WIN_W32BOOT_H_

#include <stdint.h>

#if defined(_WIN32)
#define HOSTCALL
#define ELFCALL __attribute__((sysv_abi))
#else
#define HOSTCALL __attribute__((ms_abi))
#define ELFCALL
#endif

#define W32BOOT_VERSION 1

/* win7 puts a 64-bit thread's fs base at low32(TEB + 0x2000) and android reads its canary at fs:0x28.
   so we just live there */
#define W32BOOT_FS_REGION_START 0xFF000000u
#define W32BOOT_FS_REGION_END   0xFFFF0000u

#define W32BOOT_TLS_SLOT 63

struct w32boot {
    uint32_t version;
    uint32_t size;
    void *(HOSTCALL *GetModuleHandleW)(const uint16_t *name);
    void *(HOSTCALL *LoadLibraryW)(const uint16_t *name);
    void *(HOSTCALL *GetProcAddress)(void *module, const char *name);
    int argc;
    char **argv;
    const char *exe_dir;
    uintptr_t libc_base;
    const void *libc_phdr;
    uint16_t libc_phnum;
    uintptr_t stack_low, stack_high;
};

typedef int (ELFCALL *w32_libc_start_fn)(struct w32boot *boot);

#endif

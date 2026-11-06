#ifndef BLOX7_WIN_BOOT_H_
#define BLOX7_WIN_BOOT_H_

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

void boot_set_product(const wchar_t *name);
void boot_prepare(void);
__attribute__((noreturn)) void boot_fail(const wchar_t *what, DWORD error);
int boot_run(const wchar_t *bin_dir, int argc, wchar_t **argv);

#ifdef __cplusplus
}
#endif

#endif

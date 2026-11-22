#ifndef BLOX7_WIN_ISA_H_
#define BLOX7_WIN_ISA_H_

#include <stdint.h>

struct w32boot;

void isa_start(struct w32boot *boot, uintptr_t libc_base, uintptr_t code_lo, uintptr_t code_hi);

#endif

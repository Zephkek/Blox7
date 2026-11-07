/*
 * Copyright (C) 2008 The Android Open Source Project
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#pragma once

// bionic's tls.h but the x86_64 thread pointer moved off %fs. build.py puts this dir first

#include <sys/cdefs.h>

#if defined(__aarch64__)

static inline void** __get_tls(void) {
  void** result;
  __asm__("mrs %0, tpidr_el0" : "=r"(result));
  return result;
}

static inline void __set_tls(void* tls) {
  __asm__("msr tpidr_el0, %0" : : "r" (tls));
}

#elif defined(__arm__)

static inline void** __get_tls(void) {
  void** result;
  __asm__("mrc p15, 0, %0, c13, c0, 3" : "=r"(result));
  return result;
}

// arm32 requires a syscall to set the thread pointer.
// By historical accident it's public API, but not in any header except this one.
__BEGIN_DECLS
int __set_tls(void* tls);
__END_DECLS

#elif defined(__i386__)

static inline void** __get_tls(void) {
  void** result;
  __asm__("movl %%gs:0, %0" : "=r"(result));
  return result;
}

// x86 is really hairy, so we keep that out of line.
__BEGIN_DECLS
int __set_tls(void* tls);
__END_DECLS

#elif defined(__riscv)

static inline void** __get_tls(void) {
  void** result;
  __asm__("mv %0, tp" : "=r"(result));
  return result;
}

static inline void __set_tls(void* tls) {
  __asm__("mv tp, %0" : : "r"(tls));
}

#elif defined(__x86_64__)

// windows owns %fs and %gs, so it's a TEB tls slot libc.so grabs at start. still one mov
#define W32_TLS_SLOT 63
#define W32_TLS_TEB_OFFSET (0x1480 + 8 * W32_TLS_SLOT)

static inline void** __get_tls(void) {
  void** result;
  __asm__("mov %%gs:%c1, %0" : "=r"(result) : "i"(W32_TLS_TEB_OFFSET));
  return result;
}

static inline int __set_tls(void* tls) {
  __asm__ volatile("mov %0, %%gs:%c1" : : "r"(tls), "i"(W32_TLS_TEB_OFFSET) : "memory");
  return 0;
}

#else
#error unsupported architecture
#endif

#include <platform/bionic/tls_defines.h>

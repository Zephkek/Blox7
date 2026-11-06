// GetThreadContext writes the whole CONTEXT, so buffers get 1536 bytes and 16 alignment or else
#pragma once

#include <stdint.h>

struct WinContext {
  uint64_t p_home[6];
  uint32_t context_flags;
  uint32_t mx_csr;
  uint16_t seg_cs, seg_ds, seg_es, seg_fs, seg_gs, seg_ss;
  uint32_t eflags;
  uint64_t dr[6];
  uint64_t rax, rcx, rdx, rbx, rsp, rbp, rsi, rdi;
  uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
  uint64_t rip;
  uint8_t flt_save[512];
};

struct WinExceptionRecord {
  uint32_t code, flags;
  WinExceptionRecord* record;
  void* address;
  uint32_t parameter_count, pad;
  uintptr_t information[15];
};

struct WinExceptionPointers {
  WinExceptionRecord* record;
  WinContext* context;
};

constexpr uint32_t kWinContextDebugRegisters = 0x00100010;

    .text

// void w32_run_on_stack(void* stack_top, void (*fn)(void*, void*), void* a, void* b, void** saved_rsp)
    .globl w32_run_on_stack
    .hidden w32_run_on_stack
    .type w32_run_on_stack, @function
w32_run_on_stack:
    .cfi_startproc
    movq    %rsp, (%r8)
    movq    %rdi, %rsp
    andq    $-16, %rsp
    movq    %rsi, %rax
    movq    %rdx, %rdi
    movq    %rcx, %rsi
    // nobody unwinds past this point
    .cfi_undefined %rip
    callq   *%rax
    ud2
    .cfi_endproc
    .size w32_run_on_stack, . - w32_run_on_stack

// void w32_leave_stack(void* saved_rsp, void (*fn)(void*, void*, size_t), void* a, void* b, size_t c)
    .globl w32_leave_stack
    .hidden w32_leave_stack
    .type w32_leave_stack, @function
w32_leave_stack:
    .cfi_startproc
    movq    %rdi, %rsp
    andq    $-16, %rsp
    movq    %rsi, %rax
    movq    %rdx, %rdi
    movq    %rcx, %rsi
    movq    %r8, %rdx
    .cfi_undefined %rip
    callq   *%rax
    ud2
    .cfi_endproc
    .size w32_leave_stack, . - w32_leave_stack

// bionic wants this symbol, nothing ever returns through it
    .globl __restore_rt
    .hidden __restore_rt
    .type __restore_rt, @function
__restore_rt:
    ud2
    .size __restore_rt, . - __restore_rt

    .section .note.GNU-stack, "", @progbits

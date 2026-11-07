// ms_abi exception filter for the syscall sites published by w32_vex.cpp
    .text
    .globl vex_handler
    .hidden vex_handler
    .type vex_handler, @function
vex_handler:
    movq    (%rcx), %rax                // ExceptionRecord
    cmpl    $0x80000004, (%rax)         // STATUS_SINGLE_STEP?
    jne     .Lsearch

    movq    8(%rcx), %rdx               // ContextRecord
    movq    0xf8(%rdx), %r8             // Context.Rip
    movl    g_vex_site_count(%rip), %eax
    testl   %eax, %eax
    jz      .Lsearch

    cmpq    g_vex_site_addr(%rip), %r8
    je      .Lsite0
    cmpl    $1, %eax
    je      .Lsearch
    cmpq    g_vex_site_addr+8(%rip), %r8
    je      .Lsite1
    cmpl    $2, %eax
    je      .Lsearch
    cmpq    g_vex_site_addr+16(%rip), %r8
    je      .Lsite2

.Lsearch:
    xorl    %eax, %eax                  // EXCEPTION_CONTINUE_SEARCH
    ret
.Lsite0:
    xorl    %edx, %edx
    jmp     w32_vex_hit
.Lsite1:
    movl    $1, %edx
    jmp     w32_vex_hit
.Lsite2:
    movl    $2, %edx
    jmp     w32_vex_hit

    .size vex_handler, . - vex_handler

    .data
    .globl g_vex_site_count
    .hidden g_vex_site_count
    .type g_vex_site_count, @object
    .size g_vex_site_count, 4
g_vex_site_count:
    .long 0

    .globl g_vex_site_addr
    .hidden g_vex_site_addr
    .type g_vex_site_addr, @object
    .size g_vex_site_addr, 24
g_vex_site_addr:
    .quad 0, 0, 0

    .section .note.GNU-stack,"",@progbits

#define MEMMOVE memmove_generic
#define memcpy memcpy_generic
#define __memcpy_chk __memcpy_chk_generic
#include "arch-x86_64/string/sse2-memmove-slm.S"
#undef memcpy
#undef __memcpy_chk
#undef cfi_startproc
#undef cfi_endproc

	.hidden	memmove_generic
	.hidden	memcpy_generic
	.hidden	__memcpy_chk_generic

	.text
	.globl	__memcpy_chk
	.type	__memcpy_chk, @function
	.p2align 4
__memcpy_chk:
	.cfi_startproc
	cmpq	%rcx, %rdx
	ja	__memcpy_chk_fail
	.cfi_endproc
	.size	__memcpy_chk, .-__memcpy_chk

	.globl	memmove
	.type	memmove, @function
	.globl	memcpy
	.type	memcpy, @function
memmove:
memcpy:
	.cfi_startproc
	cmpb	$0, g_w32_memmove_avx2(%rip)
	je	memmove_generic
	.cfi_endproc
	.size	memmove, .-memmove
	.size	memcpy, .-memcpy

	.hidden	memmove_avx2
	.type	memmove_avx2, @function
memmove_avx2:
	.cfi_startproc
	movq	%rdi, %rax
	cmpq	$32, %rdx
	jb	L(under_32)
	cmpq	$64, %rdx
	ja	L(over_64)
	vmovdqu	(%rsi), %ymm0
	vmovdqu	-32(%rsi, %rdx), %ymm1
	vmovdqu	%ymm0, (%rdi)
	vmovdqu	%ymm1, -32(%rdi, %rdx)
	vzeroupper
	ret

	.p2align 4
L(under_32):
	cmpl	$16, %edx
	jae	L(16_to_31)
	cmpl	$8, %edx
	jae	L(8_to_15)
	cmpl	$4, %edx
	jae	L(4_to_7)
	cmpl	$1, %edx
	ja	L(2_to_3)
	jb	L(done)
	movzbl	(%rsi), %ecx
	movb	%cl, (%rdi)
L(done):
	ret

	.p2align 4
L(16_to_31):
	vmovdqu	(%rsi), %xmm0
	vmovdqu	-16(%rsi, %rdx), %xmm1
	vmovdqu	%xmm0, (%rdi)
	vmovdqu	%xmm1, -16(%rdi, %rdx)
	ret

	.p2align 4
L(8_to_15):
	movq	(%rsi), %rcx
	movq	-8(%rsi, %rdx), %rsi
	movq	%rcx, (%rdi)
	movq	%rsi, -8(%rdi, %rdx)
	ret

	.p2align 4
L(4_to_7):
	movl	(%rsi), %ecx
	movl	-4(%rsi, %rdx), %esi
	movl	%ecx, (%rdi)
	movl	%esi, -4(%rdi, %rdx)
	ret

	.p2align 4
L(2_to_3):
	movzwl	(%rsi), %ecx
	movzwl	-2(%rsi, %rdx), %esi
	movw	%cx, (%rdi)
	movw	%si, -2(%rdi, %rdx)
	ret

	.p2align 4
L(over_64):
	cmpq	$256, %rdx
	ja	L(over_256)
	vmovdqu	(%rsi), %ymm0
	vmovdqu	32(%rsi), %ymm1
	vmovdqu	-32(%rsi, %rdx), %ymm2
	vmovdqu	-64(%rsi, %rdx), %ymm3
	cmpq	$128, %rdx
	jbe	L(last_4)
	vmovdqu	64(%rsi), %ymm4
	vmovdqu	96(%rsi), %ymm5
	vmovdqu	-96(%rsi, %rdx), %ymm6
	vmovdqu	-128(%rsi, %rdx), %ymm7
	vmovdqu	%ymm4, 64(%rdi)
	vmovdqu	%ymm5, 96(%rdi)
	vmovdqu	%ymm6, -96(%rdi, %rdx)
	vmovdqu	%ymm7, -128(%rdi, %rdx)
L(last_4):
	vmovdqu	%ymm0, (%rdi)
	vmovdqu	%ymm1, 32(%rdi)
	vmovdqu	%ymm2, -32(%rdi, %rdx)
	vmovdqu	%ymm3, -64(%rdi, %rdx)
	vzeroupper
	ret

	.p2align 4
L(over_256):
	movq	%rdi, %rcx
	subq	%rsi, %rcx
	cmpq	%rdx, %rcx
	jb	L(backward)
	cmpq	__x86_shared_cache_size_half(%rip), %rdx
	jae	L(forward_big)
L(forward):
	vmovdqu	(%rsi), %ymm4
	vmovdqu	-32(%rsi, %rdx), %ymm5
	vmovdqu	-64(%rsi, %rdx), %ymm6
	vmovdqu	-96(%rsi, %rdx), %ymm7
	vmovdqu	-128(%rsi, %rdx), %ymm8
	leaq	-128(%rdi, %rdx), %r9
	leaq	32(%rdi), %r8
	andq	$-32, %r8
	subq	%rdi, %rsi
	.p2align 4
L(forward_loop):
	vmovdqu	(%r8, %rsi), %ymm0
	vmovdqu	32(%r8, %rsi), %ymm1
	vmovdqu	64(%r8, %rsi), %ymm2
	vmovdqu	96(%r8, %rsi), %ymm3
	vmovdqa	%ymm0, (%r8)
	vmovdqa	%ymm1, 32(%r8)
	vmovdqa	%ymm2, 64(%r8)
	vmovdqa	%ymm3, 96(%r8)
	addq	$128, %r8
	cmpq	%r9, %r8
	jb	L(forward_loop)
	vmovdqu	%ymm8, (%r9)
	vmovdqu	%ymm7, 32(%r9)
	vmovdqu	%ymm6, 64(%r9)
	vmovdqu	%ymm5, 96(%r9)
	vmovdqu	%ymm4, (%rdi)
	vzeroupper
	ret

	.p2align 4
L(backward):
	testq	%rcx, %rcx
	jz	L(done)
	vmovdqu	(%rsi), %ymm4
	vmovdqu	32(%rsi), %ymm5
	vmovdqu	64(%rsi), %ymm6
	vmovdqu	96(%rsi), %ymm7
	vmovdqu	-32(%rsi, %rdx), %ymm8
	leaq	96(%rdi), %r9
	leaq	-32(%rdi, %rdx), %r8
	andq	$-32, %r8
	subq	%rdi, %rsi
	.p2align 4
L(backward_loop):
	vmovdqu	(%r8, %rsi), %ymm0
	vmovdqu	-32(%r8, %rsi), %ymm1
	vmovdqu	-64(%r8, %rsi), %ymm2
	vmovdqu	-96(%r8, %rsi), %ymm3
	vmovdqa	%ymm0, (%r8)
	vmovdqa	%ymm1, -32(%r8)
	vmovdqa	%ymm2, -64(%r8)
	vmovdqa	%ymm3, -96(%r8)
	subq	$128, %r8
	cmpq	%r9, %r8
	ja	L(backward_loop)
	vmovdqu	%ymm4, (%rdi)
	vmovdqu	%ymm5, 32(%rdi)
	vmovdqu	%ymm6, 64(%rdi)
	vmovdqu	%ymm7, 96(%rdi)
	vmovdqu	%ymm8, -32(%rdi, %rdx)
	vzeroupper
	ret

	.p2align 4
L(forward_big):
	movq	%rsi, %r8
	subq	%rdi, %r8
	cmpq	%rdx, %r8
	jb	L(forward)
	vmovdqu	(%rsi), %ymm4
	vmovdqu	-32(%rsi, %rdx), %ymm5
	vmovdqu	-64(%rsi, %rdx), %ymm6
	vmovdqu	-96(%rsi, %rdx), %ymm7
	vmovdqu	-128(%rsi, %rdx), %ymm8
	leaq	-128(%rdi, %rdx), %r9
	leaq	32(%rdi), %r8
	andq	$-32, %r8
	subq	%rdi, %rsi
	.p2align 4
L(stream_loop):
	prefetcht0	512(%r8, %rsi)
	prefetcht0	576(%r8, %rsi)
	vmovdqu	(%r8, %rsi), %ymm0
	vmovdqu	32(%r8, %rsi), %ymm1
	vmovdqu	64(%r8, %rsi), %ymm2
	vmovdqu	96(%r8, %rsi), %ymm3
	vmovntdq	%ymm0, (%r8)
	vmovntdq	%ymm1, 32(%r8)
	vmovntdq	%ymm2, 64(%r8)
	vmovntdq	%ymm3, 96(%r8)
	addq	$128, %r8
	cmpq	%r9, %r8
	jb	L(stream_loop)
	sfence
	vmovdqu	%ymm8, (%r9)
	vmovdqu	%ymm7, 32(%r9)
	vmovdqu	%ymm6, 64(%r9)
	vmovdqu	%ymm5, 96(%r9)
	vmovdqu	%ymm4, (%rdi)
	vzeroupper
	ret
	.cfi_endproc
	.size	memmove_avx2, .-memmove_avx2

	.bss
	.globl	g_w32_memmove_avx2
	.hidden	g_w32_memmove_avx2
	.type	g_w32_memmove_avx2, @object
	.size	g_w32_memmove_avx2, 1
g_w32_memmove_avx2:
	.byte	0

	.section .note.GNU-stack,"",@progbits

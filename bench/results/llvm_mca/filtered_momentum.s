	.text
	.file	"jit_signal_program_module"
	.section	.rodata.cst8,"aM",@progbits,8
	.p2align	3, 0x0
.LCPI0_0:
	.quad	0x3fe0000000000000
.LCPI0_1:
	.quad	0x3fc745d1745d1746
.LCPI0_2:
	.quad	0x3fa0c9714fbcda3b
	.text
	.globl	signal_program_func_1
	.p2align	4, 0x90
	.type	signal_program_func_1,@function
signal_program_func_1:
	.cfi_startproc
	pushq	%r15
	.cfi_def_cfa_offset 16
	pushq	%r14
	.cfi_def_cfa_offset 24
	pushq	%rbx
	.cfi_def_cfa_offset 32
	subq	$32, %rsp
	.cfi_def_cfa_offset 64
	.cfi_offset %rbx, -32
	.cfi_offset %r14, -24
	.cfi_offset %r15, -16
	movq	%rcx, %rbx
	movq	%rdi, %r15
	movq	%rsi, %rdi
	movl	%edx, %esi
	callq	jit_rt_symbol_ctx@PLT
	movq	%rax, %r14
	vmovsd	(%r15), %xmm0
	vaddsd	8(%r15), %xmm0, %xmm0
	vmulsd	.LCPI0_0(%rip), %xmm0, %xmm0
	vmovsd	%xmm0, 8(%rsp)
	vmovsd	.LCPI0_1(%rip), %xmm1
	movl	$1, %esi
	movl	$10, %edx
	movq	%rax, %rdi
	callq	jit_rt_ema_alpha@PLT
	vmovsd	%xmm0, (%rbx)
	vmovsd	.LCPI0_2(%rip), %xmm1
	movl	$1, %esi
	movl	$60, %edx
	movq	%r14, %rdi
	vmovsd	8(%rsp), %xmm0
	callq	jit_rt_ema_alpha@PLT
	vmovsd	%xmm0, 8(%rbx)
	movl	$1, %esi
	movl	$30, %edx
	movq	%r14, %rdi
	vmovsd	8(%rsp), %xmm0
	callq	jit_rt_rolling_std@PLT
	vmovsd	%xmm0, 16(%rbx)
	movl	$1, %esi
	movl	$10, %edx
	movq	%r14, %rdi
	vmovsd	8(%rsp), %xmm0
	vmovsd	.LCPI0_1(%rip), %xmm1
	callq	jit_rt_ema_alpha@PLT
	vmovsd	%xmm0, 16(%rsp)
	movl	$2, %esi
	movl	$60, %edx
	movq	%r14, %rdi
	vmovsd	8(%rsp), %xmm0
	vmovsd	.LCPI0_2(%rip), %xmm1
	callq	jit_rt_ema_alpha@PLT
	vmovsd	16(%rsp), %xmm1
	vsubsd	%xmm0, %xmm1, %xmm0
	vmovsd	%xmm0, 24(%rbx)
	movl	$1, %esi
	movl	$10, %edx
	movq	%r14, %rdi
	vmovsd	8(%rsp), %xmm0
	vmovsd	.LCPI0_1(%rip), %xmm1
	callq	jit_rt_ema_alpha@PLT
	vmovsd	%xmm0, 16(%rsp)
	movl	$2, %esi
	movl	$60, %edx
	movq	%r14, %rdi
	vmovsd	8(%rsp), %xmm0
	vmovsd	.LCPI0_2(%rip), %xmm1
	callq	jit_rt_ema_alpha@PLT
	vmovsd	%xmm0, 24(%rsp)
	movl	$3, %esi
	movl	$30, %edx
	movq	%r14, %rdi
	vmovsd	8(%rsp), %xmm0
	callq	jit_rt_rolling_std@PLT
	vmovsd	16(%rsp), %xmm1
	vucomisd	24(%rsp), %xmm1
	vxorpd	%xmm1, %xmm1, %xmm1
	jbe	.LBB0_3
	vxorpd	%xmm2, %xmm2, %xmm2
	vucomisd	%xmm2, %xmm0
	jbe	.LBB0_3
	vmovsd	.LCPI0_1(%rip), %xmm1
	movl	$4, %esi
	movl	$10, %edx
	movq	%r14, %rdi
	vmovsd	8(%rsp), %xmm0
	callq	jit_rt_ema_alpha@PLT
	vmovsd	%xmm0, 16(%rsp)
	vmovsd	.LCPI0_2(%rip), %xmm1
	movl	$5, %esi
	movl	$60, %edx
	movq	%r14, %rdi
	vmovsd	8(%rsp), %xmm0
	callq	jit_rt_ema_alpha@PLT
	vmovsd	16(%rsp), %xmm1
	vsubsd	%xmm0, %xmm1, %xmm0
	vmovsd	%xmm0, 16(%rsp)
	movl	$6, %esi
	movl	$30, %edx
	movq	%r14, %rdi
	vmovsd	8(%rsp), %xmm0
	callq	jit_rt_rolling_std@PLT
	vmovsd	16(%rsp), %xmm1
	vdivsd	%xmm0, %xmm1, %xmm1
.LBB0_3:
	vmovsd	%xmm1, 32(%rbx)
	addq	$32, %rsp
	.cfi_def_cfa_offset 32
	popq	%rbx
	.cfi_def_cfa_offset 24
	popq	%r14
	.cfi_def_cfa_offset 16
	popq	%r15
	.cfi_def_cfa_offset 8
	retq
.Lfunc_end0:
	.size	signal_program_func_1, .Lfunc_end0-signal_program_func_1
	.cfi_endproc

	.section	".note.GNU-stack","",@progbits

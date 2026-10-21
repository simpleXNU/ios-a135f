/*
 * a135f boot shim entry: arm64 Linux Image header so the Samsung bootloader
 * treats us like a kernel. It enters with the MMU/D-cache off, x0 = DTB
 * pointer, at EL1 or EL2 (we handle both and always hand XNU an EL1 CPU).
 */
	.section .text.head, "ax"
	.globl _head
_head:
	b	_start			/* code0 */
	.long	0			/* code1 */
	.quad	0			/* text_offset */
	.quad	_end			/* image_size (linked at 0) */
	.quad	0xa			/* flags: LE, 4K pages, place anywhere */
	.quad	0			/* res2 */
	.quad	0			/* res3 */
	.quad	0			/* res4 */
	.ascii	"ARM\x64"		/* magic */
	.long	0			/* res5 (PE offset) */

	.text
	.globl _start
_start:
	msr	daifset, #0xf
	mov	x19, x0			/* DTB pointer from the bootloader */
	mrs	x1, CurrentEL
	lsr	x1, x1, #2
	adr	x2, shim_entry_el
	str	x1, [x2]
	cmp	x1, #2
	b.ne	1f
	/* EL2 -> EL1: no resident hypervisor is assumed. */
	mrs	x1, midr_el1
	msr	vpidr_el2, x1		/* EL1 reads of MIDR/MPIDR return these */
	mrs	x1, mpidr_el1
	msr	vmpidr_el2, x1
	mov	x1, #(1 << 31)		/* HCR_EL2.RW: EL1 is AArch64, nothing trapped */
	msr	hcr_el2, x1
	msr	vttbr_el2, xzr
	mov	x1, #0x33ff		/* CPTR_EL2 RES1 only: no FP/SIMD traps */
	msr	cptr_el2, x1
	msr	mdcr_el2, xzr
	mrs	x1, cnthctl_el2
	orr	x1, x1, #3		/* EL1 may use the physical counter/timer */
	msr	cnthctl_el2, x1
	msr	cntvoff_el2, xzr
	mov	x1, #0x0800		/* SCTLR_EL1: RES1 bits, MMU/caches off, little endian */
	movk	x1, #0x30d0, lsl #16
	msr	sctlr_el1, x1
	isb
	adr	x1, 1f
	msr	elr_el2, x1
	mov	x1, #0x3c5		/* DAIF masked, EL1h */
	msr	spsr_el2, x1
	eret
1:	msr	spsel, #1
	adr	x1, _stack_top
	mov	sp, x1
	adr	x1, vectors
	msr	vbar_el1, x1
	isb
	/* zero .bss */
	adr	x1, _bss_start
	adr	x2, _bss_end
2:	cmp	x1, x2
	b.hs	3f
	str	xzr, [x1], #8
	b	2b
3:	mov	x0, x19
	bl	shim_main
4:	wfe
	b	4b

	.data
	.balign 8
	.globl shim_entry_el
shim_entry_el:	.quad	0		/* exception level the bootloader entered us at */
	.text

/*
 * Exception vectors. Each slot stashes x29/x30, loads its slot number
 * and jumps to the common path, which saves x0-x28 and calls
 * exc_handler(slot, regs). IRQs return; everything else ends in the
 * on-screen panic.
 */
.macro vent slot
	.balign	0x80
	stp	x29, x30, [sp, #-16]!
	mov	x29, #\slot
	b	exc_common
.endm

	.balign	0x800
	.globl vectors
vectors:
	vent 0
	vent 1
	vent 2
	vent 3
	vent 4
	vent 5
	vent 6
	vent 7
	vent 8
	vent 9
	vent 10
	vent 11
	vent 12
	vent 13
	vent 14
	vent 15

exc_common:
	sub	sp, sp, #(30 * 8)
	stp	x0, x1, [sp, #0]
	stp	x2, x3, [sp, #16]
	stp	x4, x5, [sp, #32]
	stp	x6, x7, [sp, #48]
	stp	x8, x9, [sp, #64]
	stp	x10, x11, [sp, #80]
	stp	x12, x13, [sp, #96]
	stp	x14, x15, [sp, #112]
	stp	x16, x17, [sp, #128]
	stp	x18, x19, [sp, #144]
	stp	x20, x21, [sp, #160]
	stp	x22, x23, [sp, #176]
	stp	x24, x25, [sp, #192]
	stp	x26, x27, [sp, #208]
	str	x28, [sp, #224]
	mov	x0, x29
	mov	x1, sp
	bl	exc_handler
	ldp	x0, x1, [sp, #0]
	ldp	x2, x3, [sp, #16]
	ldp	x4, x5, [sp, #32]
	ldp	x6, x7, [sp, #48]
	ldp	x8, x9, [sp, #64]
	ldp	x10, x11, [sp, #80]
	ldp	x12, x13, [sp, #96]
	ldp	x14, x15, [sp, #112]
	ldp	x16, x17, [sp, #128]
	ldp	x18, x19, [sp, #144]
	ldp	x20, x21, [sp, #160]
	ldp	x22, x23, [sp, #176]
	ldp	x24, x25, [sp, #192]
	ldp	x26, x27, [sp, #208]
	ldr	x28, [sp, #224]
	add	sp, sp, #(30 * 8)
	ldp	x29, x30, [sp], #16
	eret

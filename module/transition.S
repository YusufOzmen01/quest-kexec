/* SPDX-License-Identifier: GPL-2.0 */
/* Quest Pro: EL1, 4K pages, 39-bit VA. No stack use after TTBR0 switch.
 * The relocation loop follows Linux arm64's relocate_kernel.S cache rules.
 */
#include <linux/linkage.h>
#include <asm/assembler.h>
#include <asm/sysreg.h>

.text
.align 4
/* x0: private identity PGD, x1: physical control page, x2: descriptor head,
 * x3: Image entry, x4: DTB. The high module mapping remains in TTBR1.
 */
ENTRY(qkx_enter)
	msr daifset, #0xf
	/* First invalidate the old TTBR0 translations with walks disabled. */
	mrs x9, tcr_el1
	orr x9, x9, #(1 << 7)
	msr tcr_el1, x9
	isb
	dsb ishst
	tlbi vmalle1
	dsb ish
	isb
	msr ttbr0_el1, x0
	/* T0SZ=25, TG0=4K; preserve cacheability/shareability and TTBR1. */
	bic x9, x9, #0x3f
	mov x10, #25
	orr x9, x9, x10
	bic x9, x9, #(3 << 14)
	bic x9, x9, #(1 << 37)
	bic x9, x9, #(1 << 7)
	msr tcr_el1, x9
	isb
	br x1
ENDPROC(qkx_enter)

/* Copied to a separately allocated physical page. No external references. */
.align 4
ENTRY(qkx_reloc_start)
	mov x17, x3
	mov x18, x4
	mov x19, x2
	mrs x9, sctlr_el1
	ldr x10, =SCTLR_ELx_FLAGS
	bic x9, x9, x10
	pre_disable_mmu_workaround
	msr sctlr_el1, x9
	isb
	raw_dcache_line_size x15, x9
1:
	cbz x19, 5f
	/* Descriptor pages: { next, count, { source, destination }[255] }. */
	ldp x20, x21, [x19]
	add x19, x19, #16
2:
	cbz x21, 4f
	ldp x22, x23, [x19], #16
	mov x9, x23
	add x10, x23, #4096
3:
	dc ivac, x9
	add x9, x9, x15
	cmp x9, x10
	b.lo 3b
	dsb sy
	/* PAGE_SIZE bytes, both addresses page-aligned and nonoverlapping. */
	mov x11, #256
6:
	ldp x9, x10, [x22], #16
	stp x9, x10, [x23], #16
	subs x11, x11, #1
	b.ne 6b
	sub x21, x21, #1
	b 2b
4:
	mov x19, x20
	b 1b
5:
	dsb sy
	ic iallu
	dsb sy
	isb
	mov x0, x18
	mov x1, xzr
	mov x2, xzr
	mov x3, xzr
	br x17
	.ltorg
qkx_reloc_end:
ENDPROC(qkx_reloc_start)
.global qkx_reloc_end

/* Reversible test of the very same TTBR0/physical/MMU-off transition.
 * x0..x4 are passed through to the physical code. The relocation loop uses
 * x17..x23, so preserve every callee-saved register and keep the translation
 * state in x24..x27 until the original SCTLR is restored and control returns
 * high.
 */
.align 4
ENTRY(qkx_roundtrip)
	stp x19, x20, [sp, #-96]!
	stp x21, x22, [sp, #16]
	stp x23, x24, [sp, #32]
	stp x25, x26, [sp, #48]
	stp x27, x28, [sp, #64]
	str x30, [sp, #80]
	mrs x24, tcr_el1
	mrs x25, ttbr0_el1
	mrs x26, sctlr_el1
	adr x27, .Lroundtrip_return
	orr x9, x24, #(1 << 7)
	msr tcr_el1, x9
	isb
	dsb ishst
	tlbi vmalle1
	dsb ish
	isb
	msr ttbr0_el1, x0
	bic x9, x9, #0x3f
	mov x10, #25
	orr x9, x9, x10
	bic x9, x9, #(3 << 14)
	bic x9, x9, #(1 << 37)
	bic x9, x9, #(1 << 7)
	msr tcr_el1, x9
	isb
	br x1
.Lroundtrip_return:
	/* Disable TTBR0 walks before restoring the saved translation context. */
	mrs x9, tcr_el1
	orr x9, x9, #(1 << 7)
	msr tcr_el1, x9
	isb
	msr ttbr0_el1, x25
	dsb ishst
	tlbi vmalle1
	dsb ish
	isb
	msr tcr_el1, x24
	isb
	ldr x30, [sp, #80]
	ldp x27, x28, [sp, #64]
	ldp x25, x26, [sp, #48]
	ldp x23, x24, [sp, #32]
	ldp x21, x22, [sp, #16]
	ldp x19, x20, [sp], #96
	ret
ENDPROC(qkx_roundtrip)

.align 4
ENTRY(qkx_probe_phys_start)
	mrs x9, sctlr_el1
	ldr x10, =SCTLR_ELx_FLAGS
	bic x9, x9, x10
	pre_disable_mmu_workaround
	msr sctlr_el1, x9
	isb
	mrs x0, sctlr_el1
	and x0, x0, x10
	/* Result zero proves the cleared flags were read back while physical. */
	msr sctlr_el1, x26
	isb
	br x27
	.ltorg
.global qkx_probe_phys_end
qkx_probe_phys_end:
ENDPROC(qkx_probe_phys_start)

/* Appended to the copied relocation code for its reversible scratch test.
 * qkx_reloc_start leaves the completion value in x0, restores SCTLR here,
 * then returns through the high mapping saved by qkx_roundtrip.
 */
.align 4
ENTRY(qkx_reloc_return_start)
	msr sctlr_el1, x26
	isb
	br x27
.global qkx_reloc_return_end
qkx_reloc_return_end:
ENDPROC(qkx_reloc_return_start)

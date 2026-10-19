// SPDX-License-Identifier: GPL-2.0
/*
 * smmu_dump.ko — read-only dump of the live apps_smmu (apps-smmu@15000000)
 * stream-mapping and context-bank state, for Quest 2 (kona / sm8250).
 *
 * PURPOSE: snapshot, from a
 * running Android, every valid SMR -> S2CR -> context-bank, so we know which SIDs
 * map to which CBs, each CB's page-table base and translation config, and which
 * CBs are secure/TZ-owned (those read back RAZ from the non-secure side). This
 * turns the static DTS inventory into ground truth before any
 * mainline-managed-SMMU work or any content inspection.
 *
 * SAFETY — this module performs **ZERO writes** to any register. Every access is
 * a readl of SMMU *configuration* MMIO (GR0/GR1/CB register pages). That is the
 * safe category (reporting addresses is fine). It NEVER dereferences a TTBR, FAR
 * or any page the tables point at — reading page *contents* resolved from a
 * (possibly secure) domain is the EL2-hang hazard and is explicitly NOT done here
 * (that is a separate, gated step). It does not touch the GPU SMMU, does no
 * global reset, no TLB op, and does not clear FSR.
 *
 * Standalone: it does not touch quest_kexec.ko or its state. It has no
 * jump/execute/stage path at all.
 *
 * USE:  insmod smmu_dump.ko      (dumps to the kernel log, then stays loaded)
 *       dmesg | grep qkx_smmudump
 *       rmmod smmu_dump
 * insmod returns 0 on a successful dump; nonzero means it could not map/trust the
 * SMMU and dumped nothing (see the single error line).
 *
 * Register offsets/geometry are taken verbatim from this kernel's
 * drivers/iommu/arm-smmu-regs.h and arm-smmu.c, identical to loader.c's SMMU math.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/io.h>
#include <linux/sizes.h>

#define QKX_APPS_SMMU_PHYS	0x15000000ULL
#define QKX_APPS_SMMU_SIZE	0x100000UL		/* reg[0] size (1 MiB) */
#define QKX_APPS_SMMU_CB_OFF	(QKX_APPS_SMMU_SIZE / 2)	/* cb_base = base + resource_size/2 */

/* GR0 */
#define QKX_GR0_ID0		0x20
#define QKX_ID0_NUMSMRG		0xffu			/* ID0[7:0] */
#define QKX_GR0_ID1		0x24
#define QKX_ID1_PAGESIZE	(1u << 31)
#define QKX_ID1_NUMCB		0xffu			/* ID1[7:0] */
#define QKX_GR0_SMR(n)		(0x800 + ((n) << 2))
#define QKX_SMR_VALID		(1u << 31)
#define QKX_SMR_ID_SHIFT	0
#define QKX_SMR_MASK_SHIFT	16
#define QKX_SMR_FIELD		0x7fffu
#define QKX_GR0_S2CR(n)		(0xc00 + ((n) << 2))
#define QKX_S2CR_TYPE_SHIFT	16
#define QKX_S2CR_TYPE_MASK	0x3u
#define QKX_S2CR_TYPE_TRANS	0u
#define QKX_S2CR_TYPE_BYPASS	1u
#define QKX_S2CR_TYPE_FAULT	2u
#define QKX_S2CR_CBNDX_SHIFT	0
#define QKX_S2CR_CBNDX_MASK	0xffu

/* GR1 = base + (1 << pgshift); CBAR(n)=GR1+(n<<2), CBA2R(n)=GR1+0x800+(n<<2) */
#define QKX_GR1_CBAR(n)		(0x0 + ((n) << 2))
#define QKX_CBAR_TYPE_SHIFT	16
#define QKX_CBAR_TYPE_MASK	0x3u
#define QKX_CBAR_VMID_MASK	0xffu
#define QKX_GR1_CBA2R(n)	(0x800 + ((n) << 2))

/* Context-bank register page (offsets within CB(n) = cb_base + (n << pgshift)) */
#define QKX_CB_SCTLR		0x00
#define QKX_CB_ACTLR		0x04
#define QKX_CB_TTBR0_LO		0x20
#define QKX_CB_TTBR0_HI		0x24
#define QKX_CB_TTBCR		0x30
#define QKX_CB_CONTEXTIDR	0x34
#define QKX_CB_FSR		0x58
#define QKX_CB_FSYNR0		0x68
#define QKX_SCTLR_M		(1u << 0)

static const char *qkx_s2cr_type_str(u32 t)
{
	switch (t) {
	case QKX_S2CR_TYPE_TRANS:  return "TRANS";
	case QKX_S2CR_TYPE_BYPASS: return "BYPASS";
	case QKX_S2CR_TYPE_FAULT:  return "FAULT";
	default:                   return "?";
	}
}

static int __init qkx_smmudump_init(void)
{
	void __iomem *base;
	unsigned long pgshift, gr1_off, cb_base_off;
	u32 id0, id1, numsmrg, numcb;
	u32 i, nvalid = 0;
	/* Bitmap of CBs referenced by a valid TRANSLATE S2CR (numcb <= 256). */
	DECLARE_BITMAP(cb_seen, 256);

	bitmap_zero(cb_seen, 256);

	base = ioremap(QKX_APPS_SMMU_PHYS, QKX_APPS_SMMU_SIZE);
	if (!base) {
		pr_err("qkx_smmudump: ioremap(%#llx, %#lx) failed; nothing dumped\n",
		       QKX_APPS_SMMU_PHYS, QKX_APPS_SMMU_SIZE);
		return -ENOMEM;
	}

	id0 = readl_relaxed(base + QKX_GR0_ID0);
	id1 = readl_relaxed(base + QKX_GR0_ID1);
	numsmrg = id0 & QKX_ID0_NUMSMRG;
	numcb = id1 & QKX_ID1_NUMCB;
	pgshift = (id1 & QKX_ID1_PAGESIZE) ? 16 : 12;

	/* Same geometry guard as loader.c: only the 4 KB-page layout keeps every
	 * CB page inside the mapped 1 MiB window. Refuse anything else. */
	if (pgshift != 12 || numsmrg == 0 || numsmrg > 256 ||
	    numcb == 0 || numcb > 256) {
		pr_err("qkx_smmudump: unexpected ID0=%#010x ID1=%#010x (numsmrg=%u numcb=%u pgshift=%lu); refusing\n",
		       id0, id1, numsmrg, numcb, pgshift);
		iounmap(base);
		return -ENODEV;
	}
	gr1_off = 1UL << pgshift;		/* GR1 = base + 0x1000 */
	cb_base_off = QKX_APPS_SMMU_CB_OFF;	/* cb_base = base + 0x80000 */

	pr_emerg("qkx_smmudump: apps_smmu@%#llx ID0=%#010x ID1=%#010x numsmrg=%u numcb=%u pgshift=%lu — READ-ONLY dump begin\n",
		 QKX_APPS_SMMU_PHYS, id0, id1, numsmrg, numcb, pgshift);

	/* Pass 1: every valid stream-mapping group. */
	for (i = 0; i < numsmrg; i++) {
		u32 smr = readl_relaxed(base + QKX_GR0_SMR(i));
		u32 s2cr, type, cbndx;

		if (!(smr & QKX_SMR_VALID))
			continue;
		nvalid++;
		s2cr = readl_relaxed(base + QKX_GR0_S2CR(i));
		type = (s2cr >> QKX_S2CR_TYPE_SHIFT) & QKX_S2CR_TYPE_MASK;
		cbndx = (s2cr >> QKX_S2CR_CBNDX_SHIFT) & QKX_S2CR_CBNDX_MASK;

		pr_emerg("qkx_smmudump: SMR[%3u] id=%#06x mask=%#06x -> S2CR=%#010x type=%s cbndx=%u\n",
			 i,
			 (smr >> QKX_SMR_ID_SHIFT) & QKX_SMR_FIELD,
			 (smr >> QKX_SMR_MASK_SHIFT) & QKX_SMR_FIELD,
			 s2cr, qkx_s2cr_type_str(type), cbndx);

		if (type == QKX_S2CR_TYPE_TRANS && cbndx < numcb)
			set_bit(cbndx, cb_seen);
	}

	/* Pass 2: every context bank referenced by a valid TRANSLATE S2CR.
	 * A secure / TZ-owned CB reads back RAZ from the non-secure alias, so an
	 * all-zero CBAR+SCTLR+TTBR0 on a CB a live S2CR points at means "secure
	 * (invisible from NS)" — the classification this dump exists to make. */
	for (i = 0; i < numcb; i++) {
		unsigned long cb_off;
		u32 cbar, cba2r, sctlr, ttbr0_lo, ttbr0_hi, ttbcr, contextidr, fsr, fsynr0;
		u64 ttbr0;
		const char *note;

		if (!test_bit(i, cb_seen))
			continue;

		cb_off = cb_base_off + ((unsigned long)i << pgshift);
		if (cb_off + SZ_4K > QKX_APPS_SMMU_SIZE)	/* window guard (never true for numcb<=128) */
			continue;

		cbar       = readl_relaxed(base + gr1_off + QKX_GR1_CBAR(i));
		cba2r      = readl_relaxed(base + gr1_off + QKX_GR1_CBA2R(i));
		sctlr      = readl_relaxed(base + cb_off + QKX_CB_SCTLR);
		ttbr0_lo   = readl_relaxed(base + cb_off + QKX_CB_TTBR0_LO);
		ttbr0_hi   = readl_relaxed(base + cb_off + QKX_CB_TTBR0_HI);
		ttbcr      = readl_relaxed(base + cb_off + QKX_CB_TTBCR);
		contextidr = readl_relaxed(base + cb_off + QKX_CB_CONTEXTIDR);
		fsr        = readl_relaxed(base + cb_off + QKX_CB_FSR);
		fsynr0     = readl_relaxed(base + cb_off + QKX_CB_FSYNR0);
		ttbr0      = ((u64)ttbr0_hi << 32) | ttbr0_lo;

		if (!cbar && !sctlr && !ttbr0)
			note = "  <== SECURE? (RAZ from NS)";
		else if (sctlr & QKX_SCTLR_M)
			note = "  (translating, M=1)";
		else
			note = "  (identity/bypass, M=0)";

		pr_emerg("qkx_smmudump: CB[%3u] CBAR=%#010x(type=%u vmid=%#x) CBA2R=%#010x SCTLR=%#010x TTBR0=%#018llx TTBCR=%#010x CONTEXTIDR=%#010x FSR=%#010x FSYNR0=%#010x%s\n",
			 i, cbar,
			 (cbar >> QKX_CBAR_TYPE_SHIFT) & QKX_CBAR_TYPE_MASK,
			 cbar & QKX_CBAR_VMID_MASK,
			 cba2r, sctlr, ttbr0, ttbcr, contextidr, fsr, fsynr0, note);
	}

	pr_emerg("qkx_smmudump: dump end — %u valid SMRs, %u referenced CBs. NO writes performed.\n",
		 nvalid, (u32)bitmap_weight(cb_seen, 256));

	iounmap(base);
	return 0;
}

static void __exit qkx_smmudump_exit(void)
{
	/* nothing to undo — the module only read registers */
}

module_init(qkx_smmudump_init);
module_exit(qkx_smmudump_exit);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Read-only apps_smmu (0x15000000) SMR/S2CR/CB state dump for Quest 2 kona");

// SPDX-License-Identifier: GPL-2.0
/*
 * smmu_ptwalk.ko — read-only stage-1 page-table walk of ONE apps_smmu context
 * bank, for Quest 2 (kona).
 *
 * Given a context-bank index (cb=N), it reads that CB's TTBR0/TTBCR and walks the
 * ARMv8 LPAE stage-1 tables, printing the mapped IOVA -> PA ranges and attributes.
 * This answers "what do the held pages map" for a NON-SECURE master (e.g. USB,
 * display-unsec, QUP), and lets us validate what a mainline-managed SMMU would
 * inherit.
 *
 * SAFETY
 * ------
 * - It reads ONLY: SMMU config registers, and the page-TABLE descriptor memory
 *   (the translation structures). It NEVER reads a leaf data page — it reports a
 *   leaf's PA but never dereferences it. Reading page *contents* resolved from a
 *   (possibly secure) domain is the EL2-hang hazard and is not done here.
 * - It REFUSES secure context banks. Because on this unit secure CBs are NOT
 *   distinguishable by register readback (they read back like normal CBs), the
 *   refusal is by SID: if any SID mapped to the
 *   requested CB is in the DTS CP-VMID secure set, it refuses. It also refuses a
 *   CB with M=0 (identity/bypass, no tables) and any non-4 KB granule.
 * - Every page-table page PA is checked with pfn_valid() before it is read; a PA
 *   outside kernel RAM (e.g. a secure/no-map carveout) is refused, never read.
 * - Zero writes anywhere. No TLB op, no FSR clear, no GPU SMMU, no jump path.
 *   Independent of quest_kexec.ko.
 *
 * It reads LIVE tables (Android's, if run under Android), so a concurrent
 * map/unmap can tear the snapshot — acceptable for a diagnostic.
 *
 * USE:  insmod smmu_ptwalk.ko cb=15        (CB15 = USB SID 0x0)
 *       dmesg | grep qkx_ptwalk
 *       rmmod smmu_ptwalk
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/io.h>
#include <linux/sizes.h>
#include <linux/bits.h>
#include <linux/mm.h>

static int cb = -1;
module_param(cb, int, 0444);
MODULE_PARM_DESC(cb, "apps_smmu context-bank index to walk (required; e.g. 15 = USB)");

static unsigned int max_runs = 2048;
module_param(max_runs, uint, 0444);
MODULE_PARM_DESC(max_runs, "cap on emitted (coalesced) IOVA ranges");

#define QKX_APPS_SMMU_PHYS	0x15000000ULL
#define QKX_APPS_SMMU_SIZE	0x100000UL
#define QKX_APPS_SMMU_CB_OFF	(QKX_APPS_SMMU_SIZE / 2)

#define QKX_GR0_ID0		0x20
#define QKX_ID0_NUMSMRG		0xffu
#define QKX_GR0_ID1		0x24
#define QKX_ID1_PAGESIZE	(1u << 31)
#define QKX_ID1_NUMCB		0xffu
#define QKX_GR0_SMR(n)		(0x800 + ((n) << 2))
#define QKX_SMR_VALID		(1u << 31)
#define QKX_SMR_FIELD		0x7fffu
#define QKX_GR0_S2CR(n)		(0xc00 + ((n) << 2))
#define QKX_S2CR_TYPE_SHIFT	16
#define QKX_S2CR_TYPE_MASK	0x3u
#define QKX_S2CR_TYPE_TRANS	0u
#define QKX_S2CR_CBNDX_MASK	0xffu

#define QKX_CB_SCTLR		0x00
#define QKX_CB_TTBR0_LO		0x20
#define QKX_CB_TTBR0_HI		0x24
#define QKX_CB_TTBCR		0x30
#define QKX_SCTLR_M		(1u << 0)

/* TTBCR (stage-1, EAE) fields we need */
#define QKX_TTBCR_T0SZ(x)	((x) & 0x3f)
#define QKX_TTBCR_TG0(x)	(((x) >> 14) & 0x3)	/* 0=4K,1=64K,2=16K */

/* LPAE descriptor */
#define QKX_DESC_VALID		BIT_ULL(0)
#define QKX_DESC_TABLE		BIT_ULL(1)	/* with VALID: table(L<3)/page(L3); without: block */
#define QKX_DESC_OA_MASK	GENMASK_ULL(47, 12)
#define QKX_DESC_AF		BIT_ULL(10)
#define QKX_DESC_NS		BIT_ULL(5)
#define QKX_DESC_AP(d)		(((d) >> 6) & 0x3)
#define QKX_DESC_SH(d)		(((d) >> 8) & 0x3)
#define QKX_DESC_ATTRIDX(d)	(((d) >> 2) & 0x7)
#define QKX_DESC_PXN		BIT_ULL(53)
#define QKX_DESC_XN		BIT_ULL(54)

/* DTS CP-VMID secure SID set. The registers cannot distinguish these on this
 * unit, so refuse by SID. */
static const u16 qkx_secure_sid[] = {
	0x821,					/* SDE secure */
	0x593, 0x59c, 0x59d, 0x59e,		/* QCEDEV secure */
	0x1009,					/* CDSP secure */
	0x2101, 0x2103, 0x2104,			/* Venus secure */
	0x2123, 0x2124,				/* CVP secure */
};

static bool qkx_sid_is_secure(u16 sid)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(qkx_secure_sid); i++)
		if (qkx_secure_sid[i] == sid)
			return true;
	return false;
}

/* Coalesced-run emitter state */
static u64 run_iova, run_pa, run_len, run_attr;
static bool run_open;
static unsigned int runs_emitted;

static void qkx_flush_run(void)
{
	if (!run_open)
		return;
	pr_emerg("qkx_ptwalk:   IOVA %#014llx..%#014llx -> PA %#014llx len=%#llx AP=%llu AttrIdx=%llu NS=%llu XN=%llu\n",
		 run_iova, run_iova + run_len, run_pa, run_len,
		 (run_attr >> 0) & 0x3, (run_attr >> 2) & 0x7,
		 (run_attr >> 5) & 0x1, (run_attr >> 6) & 0x1);
	run_open = false;
}

/* attr key: AP[1:0] | AttrIdx[4:2] | NS[5] | XN[6] */
static u64 qkx_attr_key(u64 desc)
{
	return (QKX_DESC_AP(desc)) | (QKX_DESC_ATTRIDX(desc) << 2) |
	       (!!(desc & QKX_DESC_NS) << 5) | (!!(desc & QKX_DESC_XN) << 6);
}

static void qkx_emit_leaf(u64 iova, u64 pa, u64 span, u64 desc)
{
	u64 attr = qkx_attr_key(desc);

	if (run_open && iova == run_iova + run_len && pa == run_pa + run_len &&
	    attr == run_attr) {
		run_len += span;		/* extend contiguous run */
		return;
	}
	qkx_flush_run();
	if (runs_emitted >= max_runs) {
		if (runs_emitted == max_runs)
			pr_emerg("qkx_ptwalk:   ... (truncated at max_runs=%u)\n", max_runs);
		runs_emitted++;
		return;
	}
	run_iova = iova; run_pa = pa; run_len = span; run_attr = attr; run_open = true;
	runs_emitted++;
}

/* Recursively walk one table page. level: 0..3. Returns 0 or -errno. */
static int qkx_walk(phys_addr_t table_pa, int level, u64 iova_base,
		    unsigned int entries, int depth)
{
	__le64 *tbl;
	unsigned int i;
	u64 span = 1ULL << (12 + (3 - level) * 9);

	if (depth > 4)
		return -ELOOP;
	if (!table_pa || (table_pa & ~GENMASK_ULL(47, 0)))
		return -EFAULT;
	/* Only read page-table pages that are real kernel RAM. A PA outside RAM
	 * (secure / no-map carveout) is never touched. */
	if (!pfn_valid(PHYS_PFN(table_pa))) {
		pr_emerg("qkx_ptwalk:   refuse: table PA %#llx not in kernel RAM (pfn_valid=0)\n",
			 (u64)table_pa);
		return -EPERM;
	}
	tbl = phys_to_virt(table_pa);

	for (i = 0; i < entries; i++) {
		u64 desc = le64_to_cpu(tbl[i]);
		u64 iova = iova_base + (u64)i * span;
		u64 oa;

		if (!(desc & QKX_DESC_VALID))
			continue;

		if (level < 3 && (desc & QKX_DESC_TABLE)) {
			/* table descriptor -> next level (always 512 entries) */
			oa = desc & QKX_DESC_OA_MASK;
			qkx_walk(oa, level + 1, iova, 512, depth + 1);
		} else if ((level >= 1 && level <= 2 && !(desc & QKX_DESC_TABLE)) ||
			   (level == 3 && (desc & QKX_DESC_TABLE))) {
			/* block (L1/L2) or page (L3) leaf — report, never read it */
			oa = (desc & QKX_DESC_OA_MASK) & ~(span - 1);
			qkx_emit_leaf(iova, oa, span, desc);
		}
		/* else: reserved/invalid encoding — skip */
	}
	return 0;
}

static int __init qkx_ptwalk_init(void)
{
	void __iomem *base;
	unsigned long pgshift, cb_off;
	u32 id0, id1, numsmrg, numcb, i;
	u32 sctlr, ttbr0_lo, ttbr0_hi, ttbcr, t0sz, tg0;
	u64 ttbr0, table_pa;
	int va_bits, levels, start_level, start_shift;
	unsigned int start_entries, nsids = 0;
	bool secure = false;

	if (cb < 0) {
		pr_err("qkx_ptwalk: cb=<index> is required (e.g. cb=15); refusing\n");
		return -EINVAL;
	}

	base = ioremap(QKX_APPS_SMMU_PHYS, QKX_APPS_SMMU_SIZE);
	if (!base) {
		pr_err("qkx_ptwalk: ioremap failed\n");
		return -ENOMEM;
	}

	id0 = readl_relaxed(base + QKX_GR0_ID0);
	id1 = readl_relaxed(base + QKX_GR0_ID1);
	numsmrg = id0 & QKX_ID0_NUMSMRG;
	numcb = id1 & QKX_ID1_NUMCB;
	pgshift = (id1 & QKX_ID1_PAGESIZE) ? 16 : 12;
	if (pgshift != 12 || (u32)cb >= numcb) {
		pr_err("qkx_ptwalk: bad geometry pgshift=%lu numcb=%u cb=%d; refusing\n",
		       pgshift, numcb, cb);
		iounmap(base);
		return -ENODEV;
	}

	/* Which SIDs map to this CB? Refuse if any is secure. */
	for (i = 0; i < numsmrg; i++) {
		u32 smr = readl_relaxed(base + QKX_GR0_SMR(i));
		u32 s2cr, type, cbndx;

		if (!(smr & QKX_SMR_VALID))
			continue;
		s2cr = readl_relaxed(base + QKX_GR0_S2CR(i));
		type = (s2cr >> QKX_S2CR_TYPE_SHIFT) & QKX_S2CR_TYPE_MASK;
		cbndx = s2cr & QKX_S2CR_CBNDX_MASK;
		if (type != QKX_S2CR_TYPE_TRANS || cbndx != (u32)cb)
			continue;
		nsids++;
		if (qkx_sid_is_secure(smr & QKX_SMR_FIELD)) {
			pr_emerg("qkx_ptwalk: CB%d is mapped by SECURE SID %#x; REFUSING (CP/XPU-fenced)\n",
				 cb, smr & QKX_SMR_FIELD);
			secure = true;
		}
	}
	if (secure) { iounmap(base); return -EACCES; }
	if (!nsids)
		pr_emerg("qkx_ptwalk: warning: no valid TRANS SMR points at CB%d (walking anyway)\n", cb);

	cb_off = QKX_APPS_SMMU_CB_OFF + ((unsigned long)cb << pgshift);
	sctlr    = readl_relaxed(base + cb_off + QKX_CB_SCTLR);
	ttbr0_lo = readl_relaxed(base + cb_off + QKX_CB_TTBR0_LO);
	ttbr0_hi = readl_relaxed(base + cb_off + QKX_CB_TTBR0_HI);
	ttbcr    = readl_relaxed(base + cb_off + QKX_CB_TTBCR);
	ttbr0    = ((u64)ttbr0_hi << 32) | ttbr0_lo;
	iounmap(base);				/* done with SMMU registers */

	if (!(sctlr & QKX_SCTLR_M)) {
		pr_emerg("qkx_ptwalk: CB%d SCTLR=%#x M=0 (identity/bypass) — no tables to walk\n",
			 cb, sctlr);
		return -ENOENT;
	}
	tg0 = QKX_TTBCR_TG0(ttbcr);
	if (tg0 != 0) {
		pr_emerg("qkx_ptwalk: CB%d TTBCR=%#x TG0=%u not 4 KB; refusing\n", cb, ttbcr, tg0);
		return -ENOTSUPP;
	}
	t0sz = QKX_TTBCR_T0SZ(ttbcr);
	va_bits = 64 - t0sz;
	if (va_bits < 21 || va_bits > 48) {
		pr_emerg("qkx_ptwalk: CB%d VA bits %d out of range; refusing\n", cb, va_bits);
		return -ERANGE;
	}
	levels = DIV_ROUND_UP(va_bits - 12, 9);
	start_level = 4 - levels;
	start_shift = 12 + (3 - start_level) * 9;
	start_entries = 1u << (va_bits - start_shift);
	table_pa = ttbr0 & GENMASK_ULL(47, 0) & ~0xfffULL;	/* strip ASID[63:48] + low */

	pr_emerg("qkx_ptwalk: CB%d SCTLR=%#x TTBR0=%#018llx TTBCR=%#x (VA=%d start_level=%d entries=%u) table_pa=%#llx — WALK begin\n",
		 cb, sctlr, ttbr0, ttbcr, va_bits, start_level, start_entries, table_pa);

	run_open = false; runs_emitted = 0;
	qkx_walk(table_pa, start_level, 0, start_entries, 0);
	qkx_flush_run();

	pr_emerg("qkx_ptwalk: CB%d WALK end — %u IOVA ranges (%u SIDs map this CB). NO writes, no leaf reads.\n",
		 cb, runs_emitted, nsids);
	return 0;
}

static void __exit qkx_ptwalk_exit(void) { }

module_init(qkx_ptwalk_init);
module_exit(qkx_ptwalk_exit);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Read-only stage-1 page-table walk of one apps_smmu CB (Quest 2 kona)");

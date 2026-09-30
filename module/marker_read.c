// SPDX-License-Identifier: GPL-2.0
/* Read-only diagnostic for the Quest kexec log in unused reserved ramoops RAM. */
#include <linux/io.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>

#define QKX_LOG_PHYS 0x9ba80000ULL
#define QKX_LOG_SIZE 0x10000
#define QKX_LOG_MAGIC 0x514b584c
#define QKX_LOG_PAGES (QKX_LOG_SIZE / PAGE_SIZE)
/* Must match include/linux/qkx_diag.h in the diagnostic kernel. */
#define QKX_CTXAREA_SIZE 0x3000
#define QKX_CTXAREA_MAGIC 0x514b5843

struct qkx_ctx {
	u64 fn;
	s32 irq;
	char what[48];
	u64 since;
	u64 arg0, arg1;
};

struct qkx_ctxarea {
	u32 magic, nr_cpus, nr_kinds, rec_size;
	u64 text, last_pet;
	struct qkx_ctx rec[];
};

/* Print per-CPU "inside" records; fn is printed relative to runtime _text. */
static void qkx_dump_ctxarea(void *base)
{
	static const char * const kinds[] = { "hardirq", "sysfs-attr(unused)", "scm", "clk", "gdsc", "spmi", "cache", "cpu-pm", "irq-mask", "ufs-mmio" };
	struct qkx_ctxarea *a = base + QKX_LOG_SIZE - QKX_CTXAREA_SIZE;
	u32 cpu, k, n = 0;

	if (a->magic != QKX_CTXAREA_MAGIC || a->rec_size != sizeof(struct qkx_ctx) ||
	    a->nr_cpus > 64 || a->nr_kinds > 16 ||
	    sizeof(*a) + a->nr_cpus * a->nr_kinds * sizeof(struct qkx_ctx) > QKX_CTXAREA_SIZE) {
		pr_emerg("qkxctx: no context area (magic=%08x rec=%u)\n", a->magic, a->rec_size);
		return;
	}
	pr_emerg("qkxctx: text=%llx last_pet=%llu.%09llu\n", a->text,
		 a->last_pet / 1000000000ULL, a->last_pet % 1000000000ULL);
	for (cpu = 0; cpu < a->nr_cpus; cpu++)
		for (k = 0; k < a->nr_kinds; k++) {
			struct qkx_ctx *c = &a->rec[cpu * a->nr_kinds + k];

			if (!c->fn)
				continue;
			c->what[sizeof(c->what) - 1] = 0;
			pr_emerg("qkxctx: cpu%u %s since=%llu.%09llu irq/arg=%d what=%s fn=_text+%llx arg=%llx/%llx\n",
				 cpu, k < ARRAY_SIZE(kinds) ? kinds[k] : "?",
				 c->since / 1000000000ULL, c->since % 1000000000ULL,
				 c->irq, c->what, c->fn - a->text, c->arg0, c->arg1);
			n++;
		}
	pr_emerg("qkxctx: %u active records\n", n);
	for (cpu = 0; cpu < 8; cpu++) {
		u64 *h = (void *)a + 0x2800 + cpu * 22 * sizeof(u64);
		u64 cnt = h[0];

		if (!cnt)
			continue;
		/* oldest to newest; bit0 = user mode */
		for (k = 0; k < 7 && k < cnt; k++) {
			u64 slot = 1 + ((cnt - k) % 7) * 3;
			u64 pc = h[slot], daif = h[slot + 1], gic = h[slot + 2];

			pr_emerg("qkxpc: cpu%u tick=%llu %s %s%llx daif=%llx pmr=%x grp=%x\n",
				 cpu, cnt - k, (pc & 1) ? "user" : "kern",
				 (pc & 1) ? "" : "_text+",
				 (pc & 1) ? pc & ~1ULL : pc - a->text, daif,
				 (u32)gic, (u32)(gic >> 32));
		}
	}
}

struct qkx_log_header {
	u32 magic;
	u32 write_pos;
	u32 wraps;
	u32 reserved;
};

static bool write_test;
module_param(write_test, bool, 0);
/* Invalidate a stale log before a run so it can't be mistaken for a new one. */
static bool clear;
module_param(clear, bool, 0);

static int __init qkx_marker_read_init(void)
{
	struct page *pages[QKX_LOG_PAGES];
	struct qkx_log_header *h;
	char *ordered;
	void *base;
	u32 capacity = QKX_LOG_SIZE - QKX_CTXAREA_SIZE - sizeof(*h);
	u32 pos, wraps, used, first, out = 0;
	unsigned int i;

	for (i = 0; i < QKX_LOG_PAGES; i++)
		pages[i] = pfn_to_page((QKX_LOG_PHYS >> PAGE_SHIFT) + i);
	base = vmap(pages, QKX_LOG_PAGES, VM_MAP | VM_IOREMAP,
		    pgprot_writecombine(PAGE_KERNEL));
	if (!base)
		return -ENOMEM;
	h = base;
	if (clear) {
		WRITE_ONCE(h->magic, 0);
		wmb();
		pr_emerg("qkx_marker_read: cleared retained log\n");
		vunmap(base);
		return 0;
	}
	if (write_test) {
		static const char test[] = "QKX_WARM_RESET_RETENTION_TEST_v1\n";

		memset(base, 0, QKX_LOG_SIZE);
		memcpy(base + sizeof(*h), test, sizeof(test) - 1);
		WRITE_ONCE(h->write_pos, sizeof(test) - 1);
		WRITE_ONCE(h->wraps, 0);
		WRITE_ONCE(h->magic, QKX_LOG_MAGIC);
		wmb();
		pr_emerg("qkx_marker_read: wrote warm-reset retention test\n");
	}
	if (READ_ONCE(h->magic) != QKX_LOG_MAGIC) {
		pr_emerg("qkx_marker_read: no retained log; magic=%08x\n",
			 READ_ONCE(h->magic));
		vunmap(base);
		return 0;
	}
	pos = min_t(u32, READ_ONCE(h->write_pos), capacity - 1);
	wraps = READ_ONCE(h->wraps);
	used = wraps ? capacity : pos;
	first = wraps ? pos : 0;
	ordered = kmalloc(used + 1, GFP_KERNEL);
	if (!ordered) {
		vunmap(base);
		return -ENOMEM;
	}
	if (used) {
		u32 tail = min(used, capacity - first);
		memcpy(ordered, base + sizeof(*h) + first, tail);
		memcpy(ordered + tail, base + sizeof(*h), used - tail);
	}
	ordered[used] = '\0';
	qkx_dump_ctxarea(base);
	vunmap(base);

	pr_emerg("qkx_marker_read: retained log bytes=%u wraps=%u pos=%u\n",
		 used, wraps, pos);
	while (out < used) {
		u32 len = 0;

		while (out + len < used && len < 220 && ordered[out + len] != '\n')
			len++;
		pr_emerg("qkxlog: %.*s\n", (int)len, ordered + out);
		out += len;
		if (out < used && ordered[out] == '\n')
			out++;
	}
	kfree(ordered);
	return 0;
}

static void __exit qkx_marker_read_exit(void)
{
}

module_init(qkx_marker_read_init);
module_exit(qkx_marker_read_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Read retained Quest kexec log from reserved RAM");

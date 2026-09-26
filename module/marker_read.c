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
	u32 capacity = QKX_LOG_SIZE - sizeof(*h);
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

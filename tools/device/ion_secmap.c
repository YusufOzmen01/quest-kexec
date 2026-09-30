// SPDX-License-Identifier: GPL-2.0
/*
 * Read-only list of the physical ranges stock ION has handed to secure VMs:
 * live buffers carrying ION_FLAG_SECURE or a CP flag, and the pages parked in
 * the system heap's secure pools. Those pages stay hyp-assigned across kexec,
 * and touching one from the next kernel hangs the CPU.
 */
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/rbtree.h>
#include <linux/scatterlist.h>
#include <linux/mm.h>
#include <linux/msm_ion.h>
#include "../../oculus-linux-kernel-android/drivers/staging/android/ion/ion.h"
#include "../../oculus-linux-kernel-android/drivers/staging/android/ion/ion_system_heap.h"

#define SECURE_FLAGS	(ION_FLAG_SECURE | GENMASK(30, 17))

static phys_addr_t run_start, run_end;
static unsigned long total;

static void emit(phys_addr_t start, size_t len)
{
	total += len;
	if (run_end == start) {
		run_end += len;
		return;
	}
	if (run_end)
		pr_info("ionsec: %pa-%pa\n", &run_start, &run_end);
	run_start = start;
	run_end = start + len;
}

static void dump_pool(struct ion_page_pool *pool)
{
	struct page *page;

	mutex_lock(&pool->mutex);
	list_for_each_entry(page, &pool->high_items, lru)
		emit(page_to_phys(page), PAGE_SIZE << pool->order);
	list_for_each_entry(page, &pool->low_items, lru)
		emit(page_to_phys(page), PAGE_SIZE << pool->order);
	mutex_unlock(&pool->mutex);
}

static int __init ion_secmap_init(void)
{
	struct ion_device *idev;
	struct ion_heap *heap;
	struct rb_node *n;
	struct file *f;
	int vmid, i;

	f = filp_open("/dev/ion", O_RDONLY, 0);
	if (IS_ERR(f))
		return PTR_ERR(f);
	idev = container_of((struct miscdevice *)f->private_data,
			    struct ion_device, dev);

	mutex_lock(&idev->buffer_lock);
	for (n = rb_first(&idev->buffers); n; n = rb_next(n)) {
		struct ion_buffer *buf = rb_entry(n, struct ion_buffer, node);
		struct scatterlist *sg;

		if (!(buf->flags & SECURE_FLAGS) || !buf->sg_table)
			continue;
		pr_info("ionsec: buffer heap=%s flags=%#lx size=%zu\n",
			buf->heap->name, buf->flags, buf->size);
		for_each_sg(buf->sg_table->sgl, sg, buf->sg_table->nents, i)
			emit(sg_phys(sg), sg->length);
	}
	mutex_unlock(&idev->buffer_lock);

	down_read(&idev->lock);
	plist_for_each_entry(heap, &idev->heaps, node) {
		struct ion_system_heap *sys;

		if (heap->type != ION_HEAP_TYPE_SYSTEM)
			continue;
		sys = container_of(heap, struct ion_system_heap, heap);
		for (vmid = 0; vmid < VMID_LAST; vmid++)
			for (i = 0; i < MAX_ORDER; i++)
				if (sys->secure_pools[vmid][i])
					dump_pool(sys->secure_pools[vmid][i]);
	}
	up_read(&idev->lock);

	emit(0, 0);
	pr_info("ionsec: total %lu KiB\n", total >> 10);
	filp_close(f, NULL);
	return -EAGAIN;
}
module_init(ion_secmap_init);
MODULE_LICENSE("GPL");

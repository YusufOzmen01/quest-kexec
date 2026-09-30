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
#include <linux/kprobes.h>
#include "kgsl_device.h"

/* Neither is exported; resolved at load time through a kprobe address. */
static struct kgsl_driver *kgsl_drv;

static unsigned long lookup(const char *name)
{
	static unsigned long (*kln)(const char *);

	if (!kln) {
		struct kprobe kp = { .symbol_name = "kallsyms_lookup_name" };

		if (register_kprobe(&kp))
			return 0;
		kln = (void *)kp.addr;
		unregister_kprobe(&kp);
	}
	return kln(name);
}

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

static void dump_buffer(struct ion_buffer *buf, const char *what)
{
	struct scatterlist *sg;
	int i;

	if (!(buf->flags & SECURE_FLAGS) || !buf->sg_table)
		return;
	pr_info("ionsec: %s heap=%s flags=%#lx size=%zu\n", what,
		buf->heap->name, buf->flags, buf->size);
	for_each_sg(buf->sg_table->sgl, sg, buf->sg_table->nents, i)
		emit(sg_phys(sg), sg->length);
}

static unsigned long sg_total(struct sg_table *sgt)
{
	struct scatterlist *sg;
	unsigned long n = 0;
	int i;

	for_each_sg(sgt->sgl, sg, sgt->nents, i) {
		emit(sg_phys(sg), sg->length);
		n += sg->length;
	}
	return n;
}

/* The GPU driver locks its secure buffers to VMID_CP_PIXEL on its own. */
static void dump_kgsl(void)
{
	struct kgsl_process_private *private;
	struct kgsl_global_memdesc *md;
	struct kgsl_device *device;
	struct kgsl_mem_entry *entry;
	struct page **guard;
	unsigned long found = 0;
	int id;

	kgsl_drv = (struct kgsl_driver *)lookup("kgsl_driver");
	if (!kgsl_drv) {
		pr_info("ionsec: kgsl_driver not found\n");
		return;
	}
	device = kgsl_drv->devp[0];
	guard = (struct page **)lookup("kgsl_secure_guard_page");
	if (guard && *guard) {
		emit(page_to_phys(*guard), PAGE_SIZE);
		found += PAGE_SIZE;
	}
	if (device)
		list_for_each_entry(md, &device->globals, node)
			if ((md->memdesc.flags & KGSL_MEMFLAGS_SECURE) &&
			    md->memdesc.sgt)
				found += sg_total(md->memdesc.sgt);

	spin_lock(&kgsl_drv->proclist_lock);
	list_for_each_entry(private, &kgsl_drv->process_list, list) {
		spin_lock(&private->mem_lock);
		idr_for_each_entry(&private->mem_idr, entry, id)
			if ((entry->memdesc.flags & KGSL_MEMFLAGS_SECURE) &&
			    entry->memdesc.sgt)
				found += sg_total(entry->memdesc.sgt);
		spin_unlock(&private->mem_lock);
	}
	spin_unlock(&kgsl_drv->proclist_lock);

	pr_info("ionsec: kgsl secure found %lu KiB, driver counts %ld KiB\n",
		found >> 10, atomic_long_read(&kgsl_drv->stats.secure) >> 10);
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
	pr_info("ionsec: begin\n");

	mutex_lock(&idev->buffer_lock);
	for (n = rb_first(&idev->buffers); n; n = rb_next(n))
		dump_buffer(rb_entry(n, struct ion_buffer, node), "buffer");
	mutex_unlock(&idev->buffer_lock);

	down_read(&idev->lock);
	plist_for_each_entry(heap, &idev->heaps, node) {
		struct ion_system_heap *sys;
		struct ion_buffer *buf;

		/* Freed buffers wait here, still assigned, for the free thread. */
		if (heap->flags & ION_HEAP_FLAG_DEFER_FREE) {
			spin_lock(&heap->free_lock);
			list_for_each_entry(buf, &heap->free_list, list)
				dump_buffer(buf, "deferred");
			spin_unlock(&heap->free_lock);
		}
		if (heap->type != ION_HEAP_TYPE_SYSTEM)
			continue;
		sys = container_of(heap, struct ion_system_heap, heap);
		for (vmid = 0; vmid < VMID_LAST; vmid++)
			for (i = 0; i < MAX_ORDER; i++)
				if (sys->secure_pools[vmid][i])
					dump_pool(sys->secure_pools[vmid][i]);
	}
	up_read(&idev->lock);

	dump_kgsl();
	emit(0, 0);
	pr_info("ionsec: total %lu KiB\n", total >> 10);
	filp_close(f, NULL);
	return -EAGAIN;
}
module_init(ion_secmap_init);
MODULE_LICENSE("GPL");

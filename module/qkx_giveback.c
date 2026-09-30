// SPDX-License-Identifier: GPL-2.0
/*
 * Last step before kexec: hand every secure ION/kgsl page the stock kernel
 * still has assigned to another VM back to HLOS, using the stock kernel's own
 * unassign routines (the same calls it makes when freeing those buffers).
 * The stock kernel's bookkeeping is left stale on purpose; it is about to be
 * replaced. Pages the hypervisor refuses to return are logged so they can
 * still be reserved.
 */
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/rbtree.h>
#include <linux/scatterlist.h>
#include <linux/mm.h>
#include <linux/msm_ion.h>
#include <linux/kprobes.h>
#include <soc/qcom/secure_buffer.h>
#include "../../oculus-linux-kernel-android/drivers/staging/android/ion/ion.h"
#include "../../oculus-linux-kernel-android/drivers/staging/android/ion/ion_system_heap.h"
#include "kgsl_device.h"

#define SECURE_FLAGS	(ION_FLAG_SECURE | GENMASK(30, 17))

static int (*unassign_flags)(struct sg_table *sgt, unsigned long flags,
			     bool set_page_private);
static int (*assign_table)(struct sg_table *table, u32 *source_vm_list,
			   int source_nelems, int *dest_vmids,
			   int *dest_perms, int dest_nelems);
static unsigned long ok_bytes, failed_bytes;

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

static unsigned long sgt_bytes(struct sg_table *sgt)
{
	struct scatterlist *sg;
	unsigned long n = 0;
	int i;

	for_each_sg(sgt->sgl, sg, sgt->nents, i)
		n += sg->length;
	return n;
}

static void account(struct sg_table *sgt, int ret, const char *what)
{
	struct scatterlist *sg;
	int i;

	if (!ret) {
		ok_bytes += sgt_bytes(sgt);
		return;
	}
	failed_bytes += sgt_bytes(sgt);
	pr_info("giveback: %s failed %d\n", what, ret);
	for_each_sg(sgt->sgl, sg, sgt->nents, i)
		pr_info("giveback: kept 0x%llx-0x%llx\n", (u64)sg_phys(sg),
			(u64)sg_phys(sg) + sg->length);
}

/* Return one sg_table from @source to HLOS with full permissions. */
static int to_hlos(struct sg_table *sgt, u32 source)
{
	int dest = VMID_HLOS, perm = PERM_READ | PERM_WRITE | PERM_EXEC;
	int ret;

	do {
		ret = assign_table(sgt, &source, 1, &dest, &perm, 1);
	} while (ret == -EAGAIN);
	return ret;
}

static void page_to_hlos(struct page *page, size_t len, u32 source)
{
	struct scatterlist sgl;
	struct sg_table sgt = { .sgl = &sgl, .nents = 1, .orig_nents = 1 };

	sg_init_table(&sgl, 1);
	sg_set_page(&sgl, page, len, 0);
	account(&sgt, to_hlos(&sgt, source), "page");
}

static void ion_giveback(void)
{
	struct ion_device *idev;
	struct ion_heap *heap;
	struct rb_node *n;
	struct file *f;
	int vmid, i;

	f = filp_open("/dev/ion", O_RDONLY, 0);
	if (IS_ERR(f)) {
		pr_info("giveback: no /dev/ion\n");
		return;
	}
	idev = container_of((struct miscdevice *)f->private_data,
			    struct ion_device, dev);

	mutex_lock(&idev->buffer_lock);
	for (n = rb_first(&idev->buffers); n; n = rb_next(n)) {
		struct ion_buffer *buf = rb_entry(n, struct ion_buffer, node);

		if ((buf->flags & SECURE_FLAGS) && buf->sg_table)
			account(buf->sg_table,
				unassign_flags(buf->sg_table, buf->flags, false),
				buf->heap->name);
	}
	mutex_unlock(&idev->buffer_lock);

	down_read(&idev->lock);
	plist_for_each_entry(heap, &idev->heaps, node) {
		struct ion_system_heap *sys;
		struct ion_buffer *buf;

		if (heap->flags & ION_HEAP_FLAG_DEFER_FREE) {
			spin_lock(&heap->free_lock);
			list_for_each_entry(buf, &heap->free_list, list)
				if ((buf->flags & SECURE_FLAGS) && buf->sg_table)
					account(buf->sg_table,
						unassign_flags(buf->sg_table,
							       buf->flags, false),
						"deferred");
			spin_unlock(&heap->free_lock);
		}
		if (heap->type != ION_HEAP_TYPE_SYSTEM)
			continue;
		sys = container_of(heap, struct ion_system_heap, heap);
		/* Pool pages sit assigned to the VM the pool is named after. */
		for (vmid = 0; vmid < VMID_LAST; vmid++)
			for (i = 0; i < MAX_ORDER; i++) {
				struct ion_page_pool *pool =
					sys->secure_pools[vmid][i];
				struct page *page;

				if (!pool)
					continue;
				mutex_lock(&pool->mutex);
				list_for_each_entry(page, &pool->high_items, lru)
					page_to_hlos(page,
						     PAGE_SIZE << pool->order, vmid);
				list_for_each_entry(page, &pool->low_items, lru)
					page_to_hlos(page,
						     PAGE_SIZE << pool->order, vmid);
				mutex_unlock(&pool->mutex);
			}
	}
	up_read(&idev->lock);
	filp_close(f, NULL);
}

/* kgsl locks its secure memory to VMID_CP_PIXEL alone (kgsl_sharedmem.c). */
static void kgsl_giveback(void)
{
	struct kgsl_driver *drv = (void *)lookup("kgsl_driver");
	struct page **guard = (void *)lookup("kgsl_secure_guard_page");
	struct kgsl_process_private *private;
	struct kgsl_global_memdesc *md;
	struct kgsl_mem_entry *entry;
	int id;

	if (!drv) {
		pr_info("giveback: no kgsl_driver\n");
		return;
	}
	if (guard && *guard)
		page_to_hlos(*guard, PAGE_SIZE, VMID_CP_PIXEL);
	if (drv->devp[0])
		list_for_each_entry(md, &drv->devp[0]->globals, node)
			if ((md->memdesc.flags & KGSL_MEMFLAGS_SECURE) &&
			    md->memdesc.sgt)
				account(md->memdesc.sgt,
					to_hlos(md->memdesc.sgt, VMID_CP_PIXEL),
					"kgsl global");

	/* hyp_assign sleeps, so collect under the spinlocks is not possible;
	 * userspace is stopped, so the lists no longer change. */
	list_for_each_entry(private, &drv->process_list, list)
		idr_for_each_entry(&private->mem_idr, entry, id)
			if ((entry->memdesc.flags & KGSL_MEMFLAGS_SECURE) &&
			    entry->memdesc.sgt)
				account(entry->memdesc.sgt,
					to_hlos(entry->memdesc.sgt,
						VMID_CP_PIXEL),
					"kgsl entry");
}

static int __init giveback_init(void)
{
	unassign_flags = (void *)lookup("ion_hyp_unassign_sg_from_flags");
	assign_table = (void *)lookup("hyp_assign_table");
	if (!unassign_flags || !assign_table)
		return -ENOENT;

	pr_info("giveback: begin\n");
	ion_giveback();
	kgsl_giveback();
	pr_info("giveback: returned %lu KiB, kept %lu KiB\n",
		ok_bytes >> 10, failed_bytes >> 10);
	return -EAGAIN;
}
module_init(giveback_init);
MODULE_LICENSE("GPL");

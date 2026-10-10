// SPDX-License-Identifier: GPL-2.0
/* Experimental Quest Pro ARM64 kexec module. Insertion stages by default.
 * All inputs are regular files directly under /data/local/tmp.
 * No partition/file writes and no sysfs command interface.
 */
#include <linux/cpu.h>
#include <linux/crc32.h>
#include <linux/device.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/ioport.h>
#include <linux/io.h>
#include <linux/kprobes.h>
#include <linux/ktime.h>
#include <linux/kthread.h>
#include <linux/list.h>
#include <linux/mailbox_controller.h>
#include <linux/mailbox/qmp.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <asm/cacheflush.h>
#include <asm/memory.h>
#include <asm/pgtable.h>
#include <asm/sysreg.h>
#include <asm/unaligned.h>
#include <soc/qcom/scm.h>

#define QKX_LO 0x90000000ULL
#define QKX_HI 0x93400000ULL
#define QKX_ENTRY 0x90080000ULL
#define QKX_INITRD 0x93000000ULL
#define QKX_DTB 0x93200000ULL
/* Includes held allocations that land inside the destination window. */
#define QKX_MAX_PAGES 32768
#define QKX_DESC_COUNT ((PAGE_SIZE - 16) / 16)

struct qkx_desc {
	u64 next;
	u64 count;
	struct { u64 source, destination; } entry[QKX_DESC_COUNT];
};

static char *image = "/data/local/tmp/qkx-Image";
static char *initrd = "/data/local/tmp/qkx-initramfs";
static char *dtb = "/data/local/tmp/qkx-boot.dtb";
static bool execute;
static bool preserve_watchdog;
static bool watchdog_recovery;
static bool watchdog_control;
static bool test_transition;
static bool test_full_copy;
static unsigned int test_cpus;
static unsigned int phase_delay_ms;
static bool post_shutdown_probe;
static unsigned int preflight_only;
static unsigned int core_hang_control;
static unsigned int secure_watchdog_control;
static bool warm_reset_test;
static bool flush_rpmh;
static bool suspend_syncboss;
static bool disconnect_qmp;
static char *syncboss_spi = "spi0.0";
module_param(image, charp, 0);
module_param(initrd, charp, 0);
module_param(dtb, charp, 0);
module_param(execute, bool, 0);
module_param(preserve_watchdog, bool, 0);
module_param(watchdog_recovery, bool, 0);
module_param(watchdog_control, bool, 0);
module_param(test_transition, bool, 0);
module_param(test_full_copy, bool, 0);
module_param(test_cpus, uint, 0);
module_param(phase_delay_ms, uint, 0);
module_param(post_shutdown_probe, bool, 0);
MODULE_PARM_DESC(post_shutdown_probe, "Run scratch physical roundtrip after shutdown before real jump");
module_param(preflight_only, uint, 0);
module_param(core_hang_control, uint, 0);
module_param(secure_watchdog_control, uint, 0);
module_param(warm_reset_test, bool, 0);
MODULE_PARM_DESC(warm_reset_test, "Force the running Android kernel's Qualcomm warm-reboot path; no kexec staging");
module_param(flush_rpmh, bool, 0);
MODULE_PARM_DESC(flush_rpmh, "Call the exported rpmh_flush() on the apps RSC before the final jump, mirroring the cluster-idle-enter path so the RSC hardware isn't left with stale active-set state across kexec");
module_param(suspend_syncboss, bool, 0);
MODULE_PARM_DESC(suspend_syncboss, "Suspend the syncboss MCU (see syncboss_spi) through its PM callback before kexec, stopping active GPI DMA");
module_param(syncboss_spi, charp, 0);
MODULE_PARM_DESC(syncboss_spi, "SPI device name of the syncboss MCU (board-specific: spi0.0 on seacliff, spi1.0 on hollywood)");
module_param(disconnect_qmp, bool, 0);
MODULE_PARM_DESC(disconnect_qmp, "Cleanly close Android AOP QMP channels and publish mcore LINK_DOWN before kexec");
MODULE_PARM_DESC(core_hang_control, "0: untouched; 1: report secure core-hang registers; 2: clear enable bit");
MODULE_PARM_DESC(secure_watchdog_control, "0: untouched; 1: report SCM availability; 2: try supported secure-watchdog disable signatures");
MODULE_PARM_DESC(phase_delay_ms, "Pause at pre-freeze checkpoints, maximum 5000 ms");
MODULE_PARM_DESC(preflight_only, "1: preflight; 2: GPU rehearsal; 3: combined CPU/physical rehearsal");
MODULE_PARM_DESC(preserve_watchdog, "Hand watchdog to target: bypass rejected secure disable");
MODULE_PARM_DESC(watchdog_recovery, "Keep apps WDT armed for an automatic diagnostic reset instead of quiescing it");
MODULE_PARM_DESC(watchdog_control, "After shutdown, latch a 10-second watchdog and spin for host timing calibration");
MODULE_PARM_DESC(execute, "Experimental irreversible jump after staging (default false)");

extern void qkx_enter(u64 pgd, u64 code, u64 descriptors, u64 entry, u64 fdt) __noreturn;
extern const char qkx_reloc_start[], qkx_reloc_end[];
extern const char qkx_probe_phys_start[], qkx_probe_phys_end[];
extern const char qkx_reloc_return_start[], qkx_reloc_return_end[];
extern unsigned long qkx_roundtrip(u64 pgd, u64 code, u64 descriptors,
				   u64 return_entry, u64 cookie);
static unsigned long (*lookup)(const char *);
static struct page **owned;
static unsigned int nr_owned, nr_copies;

#define QKX_QMP_LINK_DOWN 0xFFFF0000
#define QKX_QMP_CH_DISCONNECTED 0xFFFF0000
#define QKX_QMP_LINK_CONNECTED 2

/* Private msm_qmp.c layouts from this exact Quest kernel build. They are used
 * only to enumerate the live driver's mailboxes; state transitions themselves
 * go through the driver's qmp_shutdown() and send_irq() implementations. */
struct qkx_qmp_mbox {
	struct list_head list;
	struct mbox_controller ctrl;
	int priority;
	u32 num_assigned;
	u32 num_shutdown;
	void __iomem *desc;
	bool rx_disabled;
	bool tx_sent;
	u32 idx_in_flight;
	u32 mcore_mbox_offset;
	u32 mcore_mbox_size;
	struct qmp_pkt rx_pkt;
	struct { u32 version, features; } version;
	int local_state;
	struct mutex state_lock;
	spinlock_t tx_lock;
	struct completion link_complete;
	struct completion ch_complete;
	struct delayed_work dwork;
	void *mdev;
};

struct qkx_qmp_device {
	struct device *dev;
	const char *name;
	struct list_head mboxes;
};

struct qkx_qmp_channel_desc {
	u32 link_state;
	u32 link_state_ack;
	u32 ch_state;
	u32 ch_state_ack;
	u32 mailbox_size;
	u32 mailbox_offset;
};

struct qkx_qmp_desc {
	u32 magic;
	u32 version;
	u32 features;
	struct qkx_qmp_channel_desc ucore;
	struct qkx_qmp_channel_desc mcore;
	u32 reserved;
};

static void qkx_copy_fromio32(void *dst, void __iomem *src, size_t len)
{
	u32 *d = dst;

	while (len >= sizeof(*d)) {
		*d++ = ioread32(src);
		src += sizeof(*d);
		len -= sizeof(*d);
	}
}

static void qkx_copy_toio32(void __iomem *dst, const void *src, size_t len)
{
	const u32 *s = src;

	while (len >= sizeof(*s)) {
		iowrite32(*s++, dst);
		dst += sizeof(*s);
		len -= sizeof(*s);
	}
}
static struct qkx_desc *first_desc, *last_desc;
static void *code_page, *pgd_page;
static bool ram_found;
static atomic_t *qkx_pm_abort_suspend;
static atomic_t qkx_suppress_wakeup_abort = ATOMIC_INIT(0);

static int qkx_wakeup_suppressor(void *unused)
{
	/* freeze_processes() clears pm_abort_suspend immediately before its
	 * loop. Keep it negative while that loop runs so USB/MCU wake IRQs cannot
	 * turn this irreversible reboot into a suspend-style -EBUSY abort.
	 */
	while (!kthread_should_stop() && atomic_read(&qkx_suppress_wakeup_abort)) {
		atomic_set(qkx_pm_abort_suspend, -1000000);
		cpu_relax();
	}
	return 0;
}

static int qkx_resolve(void)
{
	struct kprobe kp = { .symbol_name = "kallsyms_lookup_name" };
	int ret = register_kprobe(&kp);
	if (ret)
		return ret;
	lookup = (void *)kp.addr;
	unregister_kprobe(&kp);
	return 0;
}

/* Hold rejected pages until unload: do not allocate/free the same page forever. */
static void *qkx_page(void)
{
	while (nr_owned < QKX_MAX_PAGES) {
		struct page *page = alloc_page(GFP_KERNEL | __GFP_ZERO | __GFP_RETRY_MAYFAIL);
		phys_addr_t pa;
		if (!page)
			return NULL;
		owned[nr_owned++] = page;
		pa = page_to_phys(page);
		if (pa < QKX_LO || pa >= QKX_HI)
			return page_address(page);
	}
	return NULL;
}

static int qkx_add(void *source, u64 dest)
{
	unsigned int n;
	if (!last_desc || last_desc->count == QKX_DESC_COUNT) {
		struct qkx_desc *d = qkx_page();
		if (!d)
			return -ENOMEM;
		if (last_desc)
			last_desc->next = virt_to_phys(d);
		else
			first_desc = d;
		last_desc = d;
	}
	n = last_desc->count++;
	last_desc->entry[n].source = virt_to_phys(source);
	last_desc->entry[n].destination = dest;
	nr_copies++;
	return 0;
}

static struct file *qkx_open(const char *path)
{
	struct file *f;
	const char *base;
	if (strncmp(path, "/data/local/tmp/", 16))
		return ERR_PTR(-EPERM);
	base = path + 16;
	if (!*base || strchr(base, '/') || !strcmp(base, ".") || !strcmp(base, ".."))
		return ERR_PTR(-EPERM);
	f = filp_open(path, O_RDONLY | O_NOFOLLOW, 0);
	if (!IS_ERR(f) && !S_ISREG(file_inode(f)->i_mode)) {
		filp_close(f, NULL);
		return ERR_PTR(-EINVAL);
	}
	return f;
}

static int qkx_read(struct file *f, void *buf, size_t size, loff_t *pos)
{
	while (size) {
		ssize_t n = kernel_read(f, buf, size, pos);
		if (n <= 0)
			return n < 0 ? n : -EIO;
		buf += n;
		size -= n;
	}
	return 0;
}

static int qkx_stage(struct file *f, u64 dest, u64 file_size, u64 mem_size)
{
	loff_t pos = 0;
	u64 off;
	u32 crc = ~0U;
	for (off = 0; off < mem_size; off += PAGE_SIZE) {
		void *page = qkx_page();
		int ret;
		if (!page)
			return -ENOMEM;
		if (off < file_size) {
			ret = qkx_read(f, page, min_t(u64, PAGE_SIZE, file_size - off), &pos);
			if (ret)
				return ret;
			crc = crc32_le(crc, page, min_t(u64, PAGE_SIZE, file_size - off));
		}
		ret = qkx_add(page, dest + off);
		if (ret)
			return ret;
	}
	pr_info("quest_kexec: segment dest=%llx file=%llu memory=%llu crc32=%08x\n",
		dest, file_size, mem_size, crc ^ ~0U);
	return 0;
}

static int qkx_ram_cb(struct resource *res, void *arg)
{
	/* walk_system_ram_res uses inclusive ends. */
	if (res->start <= QKX_LO && res->end >= QKX_HI - 1)
		ram_found = true;
	return 0;
}

static int qkx_identity_map(void)
{
	u64 *pgd, *pud;
	phys_addr_t pa;
	size_t len = qkx_reloc_end - qkx_reloc_start;
	BUILD_BUG_ON(PAGE_SIZE != 4096);
	BUILD_BUG_ON(CONFIG_ARM64_VA_BITS != 39);
	if (len > PAGE_SIZE)
		return -E2BIG;
	code_page = qkx_page();
	pgd_page = qkx_page();
	pud = qkx_page();
	if (!code_page || !pgd_page || !pud)
		return -ENOMEM;
	pgd = pgd_page;
	pa = virt_to_phys(code_page);
	/* 39-bit/4K: L1 table points to L2 table, which maps one 2M block. */
	pgd[(pa >> 30) & 511] = virt_to_phys(pud) | PUD_TYPE_TABLE;
	pud[(pa >> 21) & 511] = (pa & PMD_MASK) | PMD_TYPE_SECT |
		PMD_SECT_AF | PMD_SECT_S | PMD_ATTRINDX(MT_NORMAL);
	memcpy(code_page, qkx_reloc_start, len);
	return 0;
}

static void qkx_release(void)
{
	while (nr_owned)
		__free_page(owned[--nr_owned]);
	vfree(owned);
	owned = NULL;
	first_desc = last_desc = NULL;
}

static int qkx_test_transition(void);

static int qkx_test_cpus(void)
{
	int (*freeze)(int) = (void *)lookup("freeze_secondary_cpus");
	void (*restore)(void) = (void *)lookup("enable_nonboot_cpus");
	int (*down)(unsigned int) = (void *)lookup("cpu_down");
	int (*up)(unsigned int) = (void *)lookup("cpu_up");
	int (*affinity)(struct task_struct *, const struct cpumask *) =
		(void *)lookup("set_cpus_allowed_ptr");
	cpumask_t saved, online, removed;
	int ret, affinity_ret, cpu;
	if (!freeze || !restore || !affinity || !down || !up)
		return -ENOENT;
	cpumask_copy(&saved, &current->cpus_allowed);
	cpumask_copy(&online, cpu_online_mask);
	cpumask_clear(&removed);
	if (!cpu_online(0))
		return -ENODEV;
	ret = affinity(current, cpumask_of(0));
	if (ret)
		return ret;
	pr_info("quest_kexec: reversible CPU offline test begins; online=%u\n",
		num_online_cpus());
	if (test_cpus == 1) {
		ret = freeze(0);
	} else {
		for_each_cpu(cpu, &online) {
			if (!cpu)
				continue;
			ret = down(cpu);
			pr_info("quest_kexec: cpu_down(%d)=%d\n", cpu, ret);
			if (ret)
				break;
			cpumask_set_cpu(cpu, &removed);
		}
	}
	pr_info("quest_kexec: CPU freeze returned %d; online=%u\n", ret,
		num_online_cpus());
	if (!ret && preflight_only == 3) {
		if (num_online_cpus() != 1)
			ret = -EBUSY;
		else
			ret = qkx_test_transition();
	}
	/* Required even if freeze partially fails: balances hotplug disable. */
	if (test_cpus == 1) {
		restore();
	} else {
		for_each_cpu(cpu, &removed) {
			int up_ret = up(cpu);
			pr_info("quest_kexec: cpu_up(%d)=%d\n", cpu, up_ret);
			if (up_ret)
				ret = up_ret;
		}
	}
	if (!cpumask_equal(&online, cpu_online_mask))
		ret = -EIO;
	affinity_ret = affinity(current, &saved);
	pr_info("quest_kexec: CPU restoration complete; online=%u affinity=%d result=%d\n",
		num_online_cpus(), affinity_ret, ret);
	return ret ? ret : affinity_ret;
}

static int qkx_test_full_copy(void)
{
	struct qkx_desc *src, *head = NULL, *prev = NULL, *d;
	unsigned int i, pages = 0, lists = 0;
	unsigned long flags, result;
	u64 start, elapsed;
	size_t len = qkx_reloc_end - qkx_reloc_start;
	size_t off = ALIGN(len, 16);
	size_t end_len = qkx_reloc_return_end - qkx_reloc_return_start;
	const unsigned long cookie = 0x514b5846554c4cUL;
	if (off + end_len > PAGE_SIZE || read_sysreg(CurrentEL) != 4)
		return -EINVAL;
	/* Clone the production descriptor chain but replace EVERY destination
	 * with a separately owned page. Original source/target descriptors stay
	 * untouched; the real boot destination is never accessed here.
	 */
	for (src = first_desc; src; src = src->next ? phys_to_virt(src->next) : NULL) {
		d = qkx_page();
		if (!d)
			return -ENOMEM;
		if (prev)
			prev->next = virt_to_phys(d);
		else
			head = d;
		prev = d;
		lists++;
		for (i = 0; i < src->count; i++) {
			void *dst = qkx_page();
			const u8 *s = phys_to_virt(src->entry[i].source);
			unsigned int j;
			if (!dst)
				return -ENOMEM;
			/* Every byte initially differs, including zero-filled tails. */
			for (j = 0; j < PAGE_SIZE; j++)
				((u8 *)dst)[j] = s[j] ^ 0xff;
			d->entry[i].source = src->entry[i].source;
			d->entry[i].destination = virt_to_phys(dst);
			d->count++;
			pages++;
		}
	}
	if (pages != nr_copies || !head)
		return -EINVAL;
	memset(code_page, 0, PAGE_SIZE);
	memcpy(code_page, qkx_reloc_start, len);
	memcpy(code_page + off, qkx_reloc_return_start, end_len);
	for (i = 0; i < nr_owned; i++)
		__flush_dcache_area(page_address(owned[i]), PAGE_SIZE);
	__flush_icache_range((unsigned long)code_page, (unsigned long)code_page + PAGE_SIZE);
	pr_emerg("quest_kexec: full scratch relocation starts: %u pages, %u descriptor pages\n",
		pages, lists);
	preempt_disable();
	local_irq_save(flags);
	start = ktime_get_ns();
	result = qkx_roundtrip(virt_to_phys(pgd_page), virt_to_phys(code_page),
		virt_to_phys(head), virt_to_phys(code_page) + off, cookie);
	elapsed = ktime_get_ns() - start;
	local_irq_restore(flags);
	preempt_enable();
	if (result != cookie)
		return -EIO;
	for (d = head; d; d = d->next ? phys_to_virt(d->next) : NULL)
		for (i = 0; i < d->count; i++)
			if (memcmp(phys_to_virt(d->entry[i].source),
				   phys_to_virt(d->entry[i].destination), PAGE_SIZE)) {
				pr_err("quest_kexec: full scratch copy mismatch\n");
				return -EIO;
			}
	pr_emerg("quest_kexec: full scratch relocation VERIFIED: %u pages, %u bytes, %llu us\n",
		pages, pages * (unsigned int)PAGE_SIZE, elapsed / 1000);
	return 0;
}

static int qkx_test_transition(void)
{
	unsigned long irq_flags, result;
	unsigned int i;
	size_t probe_len = qkx_probe_phys_end - qkx_probe_phys_start;
	size_t reloc_len = qkx_reloc_end - qkx_reloc_start;
	size_t return_len = qkx_reloc_return_end - qkx_reloc_return_start;
	size_t return_off = ALIGN(reloc_len, 16);
	struct qkx_desc *desc;
	void *source, *destination;
	const unsigned long cookie = 0x514b5852454c4f43UL; /* QKXRELOC */
	if (probe_len > PAGE_SIZE || return_off + return_len > PAGE_SIZE ||
	    read_sysreg(CurrentEL) != 4)
		return -EINVAL;
	memset(code_page, 0, PAGE_SIZE);
	memcpy(code_page, qkx_probe_phys_start, probe_len);
	for (i = 0; i < nr_owned; i++)
		__flush_dcache_area(page_address(owned[i]), PAGE_SIZE);
	__flush_icache_range((unsigned long)code_page, (unsigned long)code_page + PAGE_SIZE);
	pr_info("quest_kexec: testing reversible MMU-off roundtrip\n");
	preempt_disable();
	local_irq_save(irq_flags);
	result = qkx_roundtrip(virt_to_phys(pgd_page), virt_to_phys(code_page), 0, 0, 0);
	local_irq_restore(irq_flags);
	preempt_enable();
	pr_info("quest_kexec: physical roundtrip returned; disabled SCTLR flags=%lx\n", result);
	if (result)
		return -EIO;

	source = qkx_page();
	destination = qkx_page();
	desc = qkx_page();
	if (!source || !destination || !desc)
		return -ENOMEM;
	for (i = 0; i < PAGE_SIZE; i++)
		((u8 *)source)[i] = (u8)(i * 131 + 17);
	memset(destination, 0xa5, PAGE_SIZE);
	desc->count = 1;
	desc->entry[0].source = virt_to_phys(source);
	desc->entry[0].destination = virt_to_phys(destination);

	memset(code_page, 0, PAGE_SIZE);
	memcpy(code_page, qkx_reloc_start, reloc_len);
	memcpy(code_page + return_off, qkx_reloc_return_start, return_len);
	for (i = 0; i < nr_owned; i++)
		__flush_dcache_area(page_address(owned[i]), PAGE_SIZE);
	__flush_icache_range((unsigned long)code_page, (unsigned long)code_page + PAGE_SIZE);
	pr_info("quest_kexec: testing physical relocation loop on one scratch page\n");
	preempt_disable();
	local_irq_save(irq_flags);
	result = qkx_roundtrip(virt_to_phys(pgd_page), virt_to_phys(code_page),
		virt_to_phys(desc), virt_to_phys(code_page) + return_off, cookie);
	local_irq_restore(irq_flags);
	preempt_enable();
	if (result != cookie) {
		pr_err("quest_kexec: relocation returned wrong cookie: %lx\n", result);
		return -EIO;
	}
	if (memcmp(source, destination, PAGE_SIZE)) {
		pr_err("quest_kexec: scratch relocation data mismatch\n");
		return -EIO;
	}
	pr_info("quest_kexec: physical relocation loop returned and copied scratch page correctly\n");
	return 0;
}

/* This path is deliberately separate from staging. It has not been validated
 * on Quest firmware. In particular Qualcomm DMA and watchdog shutdown need a
 * device-specific review before using execute=1 on hardware.
 */
static void qkx_phase(unsigned int phase, const char *message, bool may_pause)
{
	pr_emerg("quest_kexec: PHASE %02u: %s\n", phase, message);
	if (may_pause && phase_delay_ms)
		msleep(phase_delay_ms);
}

static int qkx_disconnect_aop_qmp(struct device *dev)
{
	struct qkx_qmp_device *mdev = dev_get_drvdata(dev);
	void (*shutdown)(struct mbox_chan *) = (void *)lookup("qmp_shutdown");
	void (*send_irq_fn)(void *) = (void *)lookup("send_irq");
	struct qkx_qmp_mbox *mbox;
	unsigned int i, wait;

	if (!mdev || !shutdown || !send_irq_fn || !mdev->name)
		return -ENOENT;
	if (list_empty(&mdev->mboxes))
		return -ENODEV;

	list_for_each_entry(mbox, &mdev->mboxes, list) {
		u32 assigned = READ_ONCE(mbox->num_assigned);
		struct qkx_qmp_desc desc;

		if (!mbox->desc || assigned > mbox->ctrl.num_chans)
			return -EINVAL;
		pr_emerg("quest_kexec: QMP %s closing %u assigned channels state=%d\n",
			 mdev->name, assigned, READ_ONCE(mbox->local_state));
		for (i = 0; i < assigned; i++)
			shutdown(&mbox->ctrl.chans[i]);

		/* qmp_shutdown() completes asynchronously in the driver's RX worker. */
		for (wait = 0; wait < 100; wait++) {
			if (READ_ONCE(mbox->local_state) == QKX_QMP_LINK_CONNECTED)
				break;
			msleep(10);
		}
		if (READ_ONCE(mbox->local_state) != QKX_QMP_LINK_CONNECTED)
			return -ETIMEDOUT;

		qkx_copy_fromio32(&desc, mbox->desc, sizeof(desc));
		desc.mcore.link_state = QKX_QMP_LINK_DOWN;
		desc.mcore.ch_state = QKX_QMP_CH_DISCONNECTED;
		desc.mcore.ch_state_ack = QKX_QMP_CH_DISCONNECTED;
		qkx_copy_toio32(mbox->desc + offsetof(struct qkx_qmp_desc, mcore),
				&desc.mcore, sizeof(desc.mcore));
		send_irq_fn(mdev);

		for (wait = 0; wait < 100; wait++) {
			if (ioread32(mbox->desc +
			    offsetof(struct qkx_qmp_desc, mcore.link_state_ack)) ==
			    QKX_QMP_LINK_DOWN)
				break;
			msleep(10);
		}
		qkx_copy_fromio32(&desc, mbox->desc, sizeof(desc));
		pr_emerg("quest_kexec: QMP %s down result mlink=%08x mack=%08x mch=%08x mcack=%08x\n",
			 mdev->name, desc.mcore.link_state,
			 desc.mcore.link_state_ack, desc.mcore.ch_state,
			 desc.mcore.ch_state_ack);
		if (desc.mcore.link_state_ack != QKX_QMP_LINK_DOWN)
			return -ETIMEDOUT;
	}
	return 0;
}

static int __maybe_unused qkx_execute(void)
{
	void (*prepare)(char *) = (void *)lookup("kernel_restart_prepare");
	void (*migrate)(void) = (void *)lookup("migrate_to_reboot_cpu");
	void (*hotplug_enable)(void) = (void *)lookup("cpu_hotplug_enable");
	void (*shutdown_cpus)(void) = (void *)lookup("machine_shutdown");
	bool *kexec_progress = (void *)lookup("kexec_in_progress");
	bool (*stuck)(void) = (void *)lookup("cpus_are_stuck_in_kernel");
	const u32 *mode = (void *)lookup("__boot_cpu_mode");
	int (*freeze_users)(void) = (void *)lookup("freeze_processes");
	void (*thaw_users)(void) = (void *)lookup("thaw_processes");
	void (*sleep_lock)(void) = (void *)lookup("lock_system_sleep");
	void (*sleep_unlock)(void) = (void *)lookup("unlock_system_sleep");
	bool *events_check_enabled = (void *)lookup("events_check_enabled");
	int (*gpu_suspend)(struct device *) = (void *)lookup("adreno_pm_suspend");
	int (*gpu_resume)(struct device *) = (void *)lookup("adreno_pm_resume");
	ssize_t (*watchdog_disable)(struct device *, struct device_attribute *,
		const char *, size_t) = (void *)lookup("wdog_disable_set");
	struct bus_type *platform_bus = (void *)lookup("platform_bus_type");
	struct bus_type *spi_bus = (void *)lookup("spi_bus_type");
	struct device *(*find_dev)(struct bus_type *, struct device *, const char *) =
		(void *)lookup("bus_find_device_by_name");
	int (*suspend_dwc3_gadget)(void *) =
		(void *)lookup("dwc3_gadget_suspend");
	int (*rpmh_flush_fn)(const struct device *) =
		(void *)lookup("rpmh_flush");
	struct device *gpu, *watchdog, *usb_core = NULL, *rpmh_dev = NULL;
	struct device *syncboss = NULL, *qmp_dev = NULL;
	struct task_struct *wakeup_suppressor = NULL;
	bool syncboss_suspended = false;
	void __iomem *wdt_regs = NULL;
	bool sleep_locked = false;
	unsigned int i;
	int ret;
	qkx_phase(1, "symbol lookups returned", true);
	if (!prepare || !migrate || !hotplug_enable || !shutdown_cpus ||
	    !stuck || !mode)
		return -ENOENT;
	pr_emerg("quest_kexec: native shutdown symbols ready; kexec_in_progress=%s\n",
		 kexec_progress ? "available" : "compiled out");
	qkx_pm_abort_suspend = (void *)lookup("pm_abort_suspend");
	if (!freeze_users || !thaw_users || !sleep_lock || !sleep_unlock ||
	    !events_check_enabled || !qkx_pm_abort_suspend || !gpu_suspend || !gpu_resume ||
	    !watchdog_disable || !platform_bus || !find_dev ||
	    !suspend_dwc3_gadget || (suspend_syncboss && !spi_bus))
		return -ENOENT;
	/* No EL2 recovery/HVC assumptions: this implementation supports EL1 boot. */
	if (read_sysreg(CurrentEL) != 4 || mode[0] != 0xe11 || mode[1] != 0xe11)
		return -EOPNOTSUPP;
	if (stuck())
		return -EBUSY;
	qkx_phase(2, "EL1 and CPU checks passed; looking up GPU", true);
	gpu = find_dev(platform_bus, NULL, "3d00000.qcom,kgsl-3d0");
	qkx_phase(3, "GPU lookup returned; looking up watchdog", true);
	watchdog = find_dev(platform_bus, NULL, "17c10000.qcom,wdt");
	usb_core = find_dev(platform_bus, NULL, "a600000.dwc3");
	if (suspend_syncboss)
		syncboss = find_dev(spi_bus, NULL, syncboss_spi);
	if (disconnect_qmp)
		qmp_dev = find_dev(platform_bus, NULL, "c300000.qcom,qmp-aop");
	if (flush_rpmh)
		rpmh_dev = find_dev(platform_bus, NULL,
			"18200000.rsc:rpmh-regulator-mmcxlvl");
	if (flush_rpmh && (!rpmh_flush_fn || !rpmh_dev)) {
		pr_emerg("quest_kexec: flush_rpmh requested but symbol=%d dev=%d; disabling\n",
			 !!rpmh_flush_fn, !!rpmh_dev);
		put_device(rpmh_dev);
		rpmh_dev = NULL;
	}
	if (!gpu || !watchdog || !usb_core || !dev_get_drvdata(usb_core) ||
	    (disconnect_qmp && (!qmp_dev || !dev_get_drvdata(qmp_dev))) ||
	    (suspend_syncboss && (!syncboss || !syncboss->driver ||
	     !syncboss->driver->pm || !syncboss->driver->pm->suspend))) {
		put_device(gpu);
		put_device(watchdog);
		put_device(usb_core);
		put_device(rpmh_dev);
		put_device(syncboss);
		put_device(qmp_dev);
		return -ENODEV;
	}
	qkx_phase(4, "devices found; mapping watchdog if requested", true);
	if (preserve_watchdog) {
		/* The exact seacliff device name above and captured DT identify this
		 * block. Only WDT0_RST is written; timeouts and enable are preserved.
		 */
		wdt_regs = ioremap(0x17c10000, SZ_4K);
		if (!wdt_regs) {
			ret = -ENOMEM;
			goto release_devices;
		}
		qkx_phase(5, "watchdog mapped; reading registers", true);
		pr_emerg("quest_kexec: preserving watchdog en=%x bark=%u bite=%u\n",
			readl_relaxed(wdt_regs + 0x08), readl_relaxed(wdt_regs + 0x10),
			readl_relaxed(wdt_regs + 0x14));
	}
	qkx_phase(6, "preflight complete; next operation freezes userspace", true);
	if (preflight_only == 1) {
		pr_emerg("quest_kexec: preflight-only test complete; returning without shutdown\n");
		ret = 0;
		goto release_devices;
	}
	qkx_phase(7, "acquiring system-sleep lock before freezing userspace", false);
	sleep_lock();
	sleep_locked = true;
	/* This is an irreversible reboot, not suspend. Android autosleep can arm
	 * wakeup checking before blocking on system_transition_mutex, leaving a
	 * stale saved_count that makes the direct freezer abort. Suppress that
	 * suspend-only abort check while holding the system-sleep lock;
	 * freeze_processes still clears pm_abort_suspend itself.
	 */
	WRITE_ONCE(*events_check_enabled, false);
	atomic_set(&qkx_suppress_wakeup_abort, 1);
	wakeup_suppressor = kthread_run(qkx_wakeup_suppressor, NULL,
					"qkx_wakeup_suppress");
	if (IS_ERR(wakeup_suppressor)) {
		ret = PTR_ERR(wakeup_suppressor);
		wakeup_suppressor = NULL;
		atomic_set(&qkx_suppress_wakeup_abort, 0);
		goto release_devices;
	}
	/* Ensure the suppressor has run before freeze_processes clears the flag. */
	while (atomic_read(qkx_pm_abort_suspend) >= 0)
		cpu_relax();
	pr_emerg("quest_kexec: system-sleep lock acquired; wakeup abort check suppressed; freezing userspace\n");
	ret = freeze_users();
	atomic_set(&qkx_suppress_wakeup_abort, 0);
	kthread_stop(wakeup_suppressor);
	wakeup_suppressor = NULL;
	atomic_set(qkx_pm_abort_suspend, 0);
	pr_emerg("quest_kexec: PHASE 08: freeze_processes returned %d\n", ret);
	/* freeze_processes already thaws everything on failure. */
	if (ret) {
		goto release_devices;
	}
	qkx_phase(9, "calling GPU suspend", false);
	ret = gpu_suspend(gpu);
	pr_emerg("quest_kexec: PHASE 10: GPU suspend returned %d\n", ret);
	if (ret) {
		gpu_resume(gpu);
		thaw_users();
		goto release_devices;
	}
	if (suspend_syncboss) {
		qkx_phase(10, "suspending Android syncboss SPI streaming", false);
		ret = syncboss->driver->pm->suspend(syncboss);
		pr_emerg("quest_kexec: syncboss suspend returned %d\n", ret);
		if (ret) {
			gpu_resume(gpu);
			thaw_users();
			goto release_devices;
		}
		syncboss_suspended = true;
	}
	if (preflight_only >= 2) {
		int resume_ret;
		if (preflight_only == 3)
			ret = qkx_test_cpus();
		resume_ret = gpu_resume(gpu);
		thaw_users();
		pr_emerg("quest_kexec: rehearsal restored userspace; GPU resume=%d test=%d\n",
			resume_ret, ret);
		if (!ret)
			ret = resume_ret;
		goto release_devices;
	}
	/* Stop gadget traffic and controller Run/Stop without tearing down the
	 * Qualcomm glue power domain. Full driver unbind hard-resets the board. */
	qkx_phase(10, "suspending Android DWC3 gadget", false);
	ret = suspend_dwc3_gadget(dev_get_drvdata(usb_core));
	pr_emerg("quest_kexec: DWC3 gadget suspend returned %d\n", ret);
	if (ret)
		panic("quest_kexec: DWC3 gadget suspend failed (%d)", ret);
	put_device(usb_core);
	usb_core = NULL;

	/* Invoke the driver's complete operation: secure SCM call, disable mutex,
	 * application watchdog, IRQ and timer teardown. No sysfs write is used.
	 */
	if (preserve_watchdog) {
		pr_emerg("quest_kexec: deferring apps-watchdog quiesce until final jump\n");
		ret = 1;
	} else {
		pr_emerg("quest_kexec: disabling secure and application watchdogs\n");
		ret = watchdog_disable(watchdog, NULL, "1", 1);
	}
	if (ret != 1) {
		if (ret >= 0)
			ret = -EIO;
		gpu_resume(gpu);
		thaw_users();
		goto release_devices;
	}
	if (disconnect_qmp) {
		qkx_phase(109, "cleanly disconnecting Android AOP QMP", false);
		ret = qkx_disconnect_aop_qmp(qmp_dev);
		pr_emerg("quest_kexec: AOP QMP disconnect returned %d\n", ret);
		if (ret)
			panic("quest_kexec: AOP QMP disconnect failed (%d)", ret);
	}
	put_device(qmp_dev);
	qmp_dev = NULL;
	put_device(gpu);
	put_device(watchdog);
	/* Match kernel_kexec(): advertise kexec before device shutdown so PCI
	 * and any other kexec-aware shutdown callbacks choose the right path. */
	if (kexec_progress)
		WRITE_ONCE(*kexec_progress, true);
	qkx_phase(11, "calling kernel_restart_prepare; irreversible shutdown", false);
	prepare(NULL);
	qkx_phase(12, "device shutdown returned; migrating to CPU0", false);
	migrate();
	/* migrate_to_reboot_cpu() disables hotplug; native kernel_kexec()
	 * reenables it immediately before machine_shutdown(). */
	hotplug_enable();
	/* freeze_secondary_cpus() rechecks pm_wakeup_pending() per CPU; a late
	 * wake IRQ during device shutdown would abort it and force a panic. */
	WRITE_ONCE(*events_check_enabled, false);
	atomic_set(qkx_pm_abort_suspend, 0);
	qkx_phase(13, "calling native machine_shutdown", false);
	shutdown_cpus();
	qkx_phase(14, "native machine_shutdown returned", false);
	if (num_online_cpus() != 1 || stuck())
		panic("quest_kexec: native CPU shutdown failed");
	if (rpmh_dev) {
		int rpmh_ret;

		/* qkx: mirror system_pm.c's system_sleep_enter(), which flushes
		 * every cached RPMh vote to the RSC's sleep/wake TCS banks before
		 * the last CPU stops running. A normal Android reboot never needs
		 * this (a real SoC reset re-inits the RSC hardware via PSHOLD/PBL
		 * before the new kernel boots); our kexec never resets the RSC at
		 * all, so without this the hardware is left holding whatever
		 * active-set state the dying kernel had, which is suspected to
		 * cause the intermittent post-kexec RPMh/SPMI stalls seen across
		 * runs. rpmh_flush() is a no-op if nothing is dirty.
		 */
		qkx_phase(140, "flushing RPMh votes to RSC before final jump", false);
		rpmh_ret = rpmh_flush_fn(rpmh_dev);
		pr_emerg("quest_kexec: rpmh_flush returned %d\n", rpmh_ret);
		qkx_phase(141, "rpmh_flush returned", false);
		put_device(rpmh_dev);
		rpmh_dev = NULL;
	}
	if (post_shutdown_probe) {
		qkx_phase(16, "post-shutdown physical scratch probe begins", false);
		msleep(50);
		ret = qkx_test_transition();
		pr_emerg("quest_kexec: PHASE 17: post-shutdown physical scratch result=%d\n", ret);
		msleep(50);
		if (ret)
			panic("quest_kexec: post-shutdown scratch probe failed (%d)", ret);
		/* Probe appended a return stub; restore the production code image. */
		memset(code_page, 0, PAGE_SIZE);
		memcpy(code_page, qkx_reloc_start, qkx_reloc_end - qkx_reloc_start);
	}
	if (wdt_regs) {
		if (watchdog_recovery) {
			/* Retain the normal 12/15-second watchdog for this diagnostic
			 * attempt so a warm reset preserves the early-entry RAM marker.
			 */
			writel_relaxed(1, wdt_regs + 0x04);
			mb();
			pr_emerg("quest_kexec: apps watchdog armed for diagnostic recovery; EN=%x\n",
				 readl_relaxed(wdt_regs + 0x08));
		} else {
			/* Stop the apps watchdog while normal mappings and USB diagnostics
			 * are still available. The target watchdog probe re-enables it.
			 */
			writel_relaxed(0, wdt_regs + 0x08);
			mb();
			pr_emerg("quest_kexec: apps watchdog quiesced; EN readback=%x\n",
				 readl_relaxed(wdt_regs + 0x08));
		}
	}
	qkx_phase(15, "single CPU; flushing payload and entering physical trampoline", false);
	if (watchdog_control) {
		/* Second calibration point: exactly twice the first control's ticks. */
		writel_relaxed(0, wdt_regs + 0x08);
		mb();
		writel_relaxed(622535, wdt_regs + 0x10);
		writel_relaxed(655300, wdt_regs + 0x14);
		writel_relaxed(3, wdt_regs + 0x08);
		writel_relaxed(1, wdt_regs + 0x04);
		mb();
		pr_emerg("quest_kexec: watchdog timing control armed; spinning\n");
		local_irq_disable();
		for (;;)
			cpu_relax();
	}
	/* Let the direct USB reader observe CPU7 completion and this checkpoint
	 * before interrupts are masked. This is not an acknowledgment protocol.
	 */
	msleep(50);
	local_irq_disable();
	preempt_disable();
	/* Everything the physical code reads must be clean to PoC. */
	for (i = 0; i < nr_owned; i++)
		__flush_dcache_area(page_address(owned[i]), PAGE_SIZE);
	__flush_icache_range((unsigned long)code_page, (unsigned long)code_page + PAGE_SIZE);
	qkx_enter(virt_to_phys(pgd_page), virt_to_phys(code_page),
		virt_to_phys(first_desc), QKX_ENTRY, QKX_DTB);
release_devices:
	if (syncboss_suspended && syncboss && syncboss->driver &&
	    syncboss->driver->pm && syncboss->driver->pm->resume)
		syncboss->driver->pm->resume(syncboss);
	if (wakeup_suppressor) {
		atomic_set(&qkx_suppress_wakeup_abort, 0);
		kthread_stop(wakeup_suppressor);
		atomic_set(qkx_pm_abort_suspend, 0);
	}
	if (sleep_locked)
		sleep_unlock();
	if (wdt_regs)
		iounmap(wdt_regs);
	put_device(gpu);
	put_device(watchdog);
	put_device(usb_core);
	put_device(rpmh_dev);
	put_device(syncboss);
	put_device(qmp_dev);
	return ret;
}

static int qkx_core_hang_configure(void)
{
	static const phys_addr_t config[] = {
		0x18000060, 0x18010060, 0x18020060, 0x18030060,
		0x18040060, 0x18050060, 0x18060060, 0x18070060,
	};
	u32 (*secure_read)(phys_addr_t) = (void *)lookup("scm_io_read");
	int (*secure_write)(phys_addr_t, u32) = (void *)lookup("scm_io_write");
	unsigned int i;

	if (!secure_read || (core_hang_control == 2 && !secure_write))
		return -ENOENT;
	for (i = 0; i < ARRAY_SIZE(config); i++) {
		u32 before = secure_read(config[i]);
		int ret = 0;
		u32 after = before;

		if (core_hang_control == 2 && (before & BIT(1))) {
			ret = secure_write(config[i], before & ~BIT(1));
			after = secure_read(config[i]);
		}
		pr_emerg("quest_kexec: core-hang cpu%u config=%pa before=%08x after=%08x write=%d\n",
			i, &config[i], before, after, ret);
		if (ret || (core_hang_control == 2 && (after & BIT(1))))
			return ret ? ret : -EIO;
	}
	return 0;
}

static int qkx_secure_watchdog_configure(void)
{
	int (*available)(u32, u32) = (void *)lookup("scm_is_call_available");
	int (*call)(u32, struct scm_desc *) = (void *)lookup("scm_call2");
	struct scm_desc desc = { };
	int avail, ret;

	if (!available || !call)
		return -ENOENT;
	avail = available(SCM_SVC_BOOT, 0x7);
	pr_emerg("quest_kexec: secure-watchdog disable SCM availability=%d\n", avail);
	if (secure_watchdog_control != 2)
		return avail < 0 ? avail : 0;

	/* This firmware rejects the downstream driver's one-argument value 1.
	 * Probe the older no-argument and one-argument-zero ABIs in that order.
	 */
	desc.arginfo = SCM_ARGS(0);
	ret = call(SCM_SIP_FNID(SCM_SVC_BOOT, 0x7), &desc);
	pr_emerg("quest_kexec: secure-watchdog disable noarg ret=%d secure=%llx/%llx/%llx\n",
		ret, desc.ret[0], desc.ret[1], desc.ret[2]);
	if (!ret)
		return 0;
	memset(&desc, 0, sizeof(desc));
	desc.arginfo = SCM_ARGS(1);
	desc.args[0] = 0;
	ret = call(SCM_SIP_FNID(SCM_SVC_BOOT, 0x7), &desc);
	pr_emerg("quest_kexec: secure-watchdog disable arg0 ret=%d secure=%llx/%llx/%llx\n",
		ret, desc.ret[0], desc.ret[1], desc.ret[2]);
	return ret;
}

static int __init qkx_init(void)
{
	struct file *kf = NULL, *rf = NULL, *df = NULL;
	u8 kh[64], dh[40];
	u64 ksize, rsize, dsize, image_size, flags;
	loff_t pos = 0;
	int ret;
	int (*walk_ram)(u64, u64, void *, int (*)(struct resource *, void *));
	if (test_cpus > 2 || preflight_only > 3 || core_hang_control > 2 ||
	    secure_watchdog_control > 2 || phase_delay_ms > 5000 ||
	    (preflight_only && !execute) || (post_shutdown_probe && !execute) ||
	    (watchdog_recovery && (!execute || !preserve_watchdog)) ||
	    (watchdog_control && !watchdog_recovery))
		return -EINVAL;
	if ((execute && (test_transition || test_cpus)) || (test_transition && test_cpus))
		return -EINVAL;
	if (test_full_copy && (execute || test_transition || test_cpus))
		return -EINVAL;
#ifndef QKX_ENABLE_EXPERIMENTAL_JUMP
	if (execute) {
		pr_err("quest_kexec: jump disabled in this staging build; board shutdown is unvalidated\n");
		return -EOPNOTSUPP;
	}
#endif
	if (!of_machine_is_compatible("qcom,kona"))
		return -ENODEV;
	ret = qkx_resolve();
	if (ret)
		return ret;
	if (warm_reset_test) {
		bool *force_warm = (void *)lookup("force_warm_reboot");
		void (*restart)(char *) = (void *)lookup("machine_restart");

		if (!force_warm || !restart)
			return -ENOENT;
		WRITE_ONCE(*force_warm, true);
		pr_emerg("quest_kexec: forcing Qualcomm warm-reset calibration now\n");
		msleep(100);
		restart(NULL);
		return -EIO;
	}
	if (core_hang_control) {
		ret = qkx_core_hang_configure();
		if (ret)
			return ret;
	}
	if (secure_watchdog_control) {
		ret = qkx_secure_watchdog_configure();
		if (ret)
			return ret;
	}
	walk_ram = (void *)lookup("walk_system_ram_res");
	if (!walk_ram)
		return -ENOENT;
	ret = walk_ram(QKX_LO, QKX_HI - 1, NULL, qkx_ram_cb);
	if (ret || !ram_found)
		return -ERANGE;
	/* Host prepare_boot.py also rejects nested reserved iomem regions. */
	owned = vzalloc(QKX_MAX_PAGES * sizeof(*owned));
	if (!owned)
		return -ENOMEM;
	kf = qkx_open(image);
	if (IS_ERR(kf)) { ret = PTR_ERR(kf); kf = NULL; goto out; }
	rf = qkx_open(initrd);
	if (IS_ERR(rf)) { ret = PTR_ERR(rf); rf = NULL; goto out; }
	df = qkx_open(dtb);
	if (IS_ERR(df)) { ret = PTR_ERR(df); df = NULL; goto out; }
	ksize = i_size_read(file_inode(kf));
	rsize = i_size_read(file_inode(rf));
	dsize = i_size_read(file_inode(df));
	ret = -EINVAL;
	if (ksize < sizeof(kh) || !rsize || rsize > SZ_2M || dsize < sizeof(dh) || dsize > SZ_2M)
		goto out;
	ret = qkx_read(kf, kh, sizeof(kh), &pos);
	if (ret)
		goto out;
	image_size = get_unaligned_le64(kh + 16);
	flags = get_unaligned_le64(kh + 24);
	ret = -EINVAL;
	if (memcmp(kh + 56, "ARM\x64", 4) || get_unaligned_le64(kh + 8) != SZ_512K ||
	    image_size < ksize || image_size > QKX_INITRD - QKX_ENTRY ||
	    (flags & 1) || !(flags & 8) || ((flags & 6) != 0 && (flags & 6) != 2))
		goto out;
	pos = 0;
	ret = qkx_read(df, dh, sizeof(dh), &pos);
	if (ret)
		goto out;
	ret = -EINVAL;
	if (get_unaligned_be32(dh) != 0xd00dfeed || get_unaligned_be32(dh + 4) != dsize)
		goto out;
	ret = qkx_stage(kf, QKX_ENTRY, ksize, PAGE_ALIGN(image_size));
	if (!ret)
		ret = qkx_stage(rf, QKX_INITRD, rsize, PAGE_ALIGN(rsize));
	if (!ret)
		ret = qkx_stage(df, QKX_DTB, dsize, PAGE_ALIGN(dsize));
	if (!ret)
		ret = qkx_identity_map();
out:
	if (kf) filp_close(kf, NULL);
	if (rf) filp_close(rf, NULL);
	if (df) filp_close(df, NULL);
	if (!ret) {
		pr_info("quest_kexec: staged %u copy pages, %u allocations; entry=%llx dtb=%llx; execute=%u\n",
			nr_copies, nr_owned, QKX_ENTRY, QKX_DTB, execute);
		if (test_transition)
			ret = qkx_test_transition();
		if (test_full_copy)
			ret = qkx_test_full_copy();
		if (test_cpus)
			ret = qkx_test_cpus();
#ifdef QKX_ENABLE_EXPERIMENTAL_JUMP
		if (execute) {
			qkx_phase(0, "entering execute path; resolving shutdown symbols", true);
			ret = qkx_execute();
		}
#endif
	}
	if (ret) {
		pr_err("quest_kexec: staging/validation failed: %d\n", ret);
		qkx_release();
	}
	return ret;
}

static void __exit qkx_exit(void)
{
	qkx_release();
	pr_info("quest_kexec: unloaded; staging memory released\n");
}
module_init(qkx_init);
module_exit(qkx_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Experimental Quest Pro ARM64 kexec staging and EL1 transition");

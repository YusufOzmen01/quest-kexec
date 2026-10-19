// SPDX-License-Identifier: GPL-2.0
/* Experimental Meta Quest (Kona) ARM64 kexec module. Insertion stages by default.
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
#include <linux/iommu.h>
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
#include "core.h" /* Matching kernel's drivers/usb/dwc3; no guessed offsets. */

#define QKX_LO 0x90000000ULL
#define QKX_HI 0x93400000ULL
/* Entry = 2 MiB-aligned base (QKX_LO) + the Image's text_offset. Downstream
 * Oculus 4.19 Images use text_offset 0x80000 (the default, giving the historical
 * 0x90080000); a mainline arm64 Image declares text_offset 0 and is placed at
 * QKX_LO. Driven by the kernel_text_offset module param below. */
#define QKX_ENTRY (QKX_LO + kernel_text_offset)
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
static unsigned long kernel_text_offset = 0x80000;
static char *bypass_devices;
static bool bypass_resolve_only;
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
module_param(kernel_text_offset, ulong, 0);
MODULE_PARM_DESC(kernel_text_offset, "ARM64 Image text_offset; entry = 0x90000000 + this. Default 0x80000 (downstream Oculus 4.19). Pass 0 for a mainline Image whose header text_offset is 0. Must equal the Image header's text_offset.");
module_param(bypass_devices, charp, 0);
MODULE_PARM_DESC(bypass_devices, "'/'-separated list of DT platform device names whose non-secure apps_smmu context bank is put into CB-level stage-1 bypass (clear SCTLR.M -> IOVA=PA) just before the jump, so a target kernel that leaves apps_smmu disabled can DMA with physical addresses. Separator is '/': it can't appear in a dev_name and is not shell-special, so the value needs no quoting through run.sh/adb/device-sh (a shell-special separator would be split by a shell layer and silently half-apply). Any char outside [alnum . , - _ @ : /] is rejected with -EINVAL. Each name is resolved to its SMMU master and flipped all-or-nothing under runtime guards (single stream, SMR mask==0, secure-SID denylist, CB-exclusivity, geometry, already-bypassed abort, readback+restore). The caller is responsible for the target being safe to bypass (quiesced, not a shared wrapper CB co-used by an active sibling) - the loader's guards are a backstop, not a vetting. The named device is resolved by walking UP to its arm-smmu master, so name the fwspec-bearing node or a descendant of it. USB only: bypass_devices=a600000.dwc3 (its parent a600000.ssusb carries the stream; the gadget is suspended anyway). USB+syncboss: bypass_devices=a600000.dwc3/8c0000.qcom,qupv3_2_geni_se plus suspend_syncboss=1 (name the QUP WRAPPER, not the SE, which is a DT sibling; no quoting needed). Default empty.");
module_param(bypass_resolve_only, bool, 0);
MODULE_PARM_DESC(bypass_resolve_only, "Dry run: resolve every bypass_devices entry read-only through all guards and log each resolved CB/SID, then stop WITHOUT writing any CB and WITHOUT jumping. For a Phase-A pre-flight to confirm a multi-entry set resolves as expected before the real run. Requires bypass_devices and execute=0; pure register reads, no device risk.");
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

/* apps_smmu (apps-smmu@15000000) GR0 register offsets, from the stock
 * drivers/iommu/arm-smmu-regs.h. The USB controller is apps_smmu stream 0x0
 * (kona-usb.dtsi: ssusb@a600000 iommus = <&apps_smmu 0x0 0x0>). A target kernel
 * that leaves apps_smmu disabled DMAs with physical addresses, but the stream is
 * still attached to the stock kernel's translating context bank, so dwc3's
 * event-buffer/TRB DMA faults. Put just that one stream's S2CR into bypass so
 * physical DMA passes through 1:1. This touches only the non-secure USB stream:
 * no global SMMU reset, no secure context bank, no arm_smmu_enable_s1_translations. */
#define QKX_APPS_SMMU_PHYS		0x15000000ULL
#define QKX_APPS_SMMU_DEVNAME		"15000000.apps-smmu"
#define QKX_SMMU_GR0_ID0		0x20
#define QKX_SMMU_ID0_NUMSMRG		0xffu
#define QKX_SMMU_GR0_SMR(n)		(0x800 + ((n) << 2))
#define QKX_SMMU_SMR_VALID		(1u << 31)
#define QKX_SMMU_SMR_ID_SHIFT		0
#define QKX_SMMU_SMR_MASK_SHIFT		16
#define QKX_SMMU_SMR_FIELD		0x7fffu	/* arm-smmu-regs.h SMR id/mask fields */
#define QKX_SMMU_GR0_S2CR(n)		(0xc00 + ((n) << 2))
#define QKX_S2CR_TYPE_SHIFT		16
#define QKX_S2CR_TYPE_MASK		0x3u
#define QKX_S2CR_TYPE_TRANS		0u
#define QKX_S2CR_CBNDX_SHIFT		0
#define QKX_S2CR_CBNDX_MASK		0xffu

/* Context-bank geometry and register offsets, matching the stock arm-smmu.c /
 * arm-smmu-regs.h this build runs. The apps_smmu register window is 1 MB
 * (DT reg = <0x15000000 0x100000>); the context-bank space starts at window/2
 * (arm-smmu.c: smmu->cb_base = base + resource_size/2), and CB(n) sits at
 * cb_base + (n << pgshift) (ARM_SMMU_CB). pgshift and the implemented bank
 * count come from ID1 at runtime. For CB-level stage-1 bypass we clear only
 * SCTLR.M on the USB stream's own context bank: that is exactly how the driver
 * composes an S1_BYPASS domain (arm-smmu.c withholds SCTLR_M for
 * DOMAIN_ATTR_S1_BYPASS), and the display CB runs this way live, so it is a
 * proven on-device mode — unlike S2CR BYPASS, which QHEE rejects on this unit. */
#define QKX_APPS_SMMU_SIZE		0x100000UL
#define QKX_APPS_SMMU_CB_BASE_OFF	(QKX_APPS_SMMU_SIZE / 2)
#define QKX_SMMU_GR0_ID1		0x24
#define QKX_SMMU_ID1_PAGESIZE		(1u << 31)
#define QKX_SMMU_ID1_NUMCB_SHIFT	0
#define QKX_SMMU_ID1_NUMCB_MASK		0xffu
#define QKX_SMMU_CB_SCTLR		0x0
#define QKX_SMMU_CB_RESUME		0x8
#define QKX_SMMU_CB_FSR			0x58
#define QKX_SMMU_CB_S1_TLBIALL		0x618
#define QKX_SMMU_CB_TLBSYNC		0x7f0
#define QKX_SMMU_CB_TLBSTATUS		0x7f4
#define QKX_SMMU_SCTLR_M		(1u << 0)
#define QKX_SMMU_FSR_SS			(1u << 30)
#define QKX_SMMU_RESUME_TERMINATE	(1u << 0)
#define QKX_SMMU_TLBSTATUS_SACTIVE	(1u << 0)
#define QKX_SMMU_SYNC_VAL		0xffffffffu	/* QCOM_DUMMY_VAL (-1) */
#define QKX_SMMU_TLBSYNC_TIMEOUT_US	500000

/* Mirror of the stock arm-smmu.c private per-master cfg reached through the
 * public iommu_fwspec->iommu_priv:
 *   struct arm_smmu_master_cfg { struct arm_smmu_device *smmu; s16 smendx[]; };
 * smendx[i] is the S2CR/SMR index the driver assigned to fwspec->ids[i]. */
struct qkx_smmu_master_cfg { void *smmu; s16 smendx[]; };

/* Secure / firmware-owned apps_smmu SIDs that must NEVER be bypassed — a safety
 * backstop so a mistaken bypass_devices entry cannot flip a secure context bank
 * (an EL2/secure hazard). No qcom,secure-vmid exists in this DT, so this
 * denylist is the mechanism; it is NOT exhaustive and does NOT replace the
 * caller's per-target vetting. Each entry is an inclusive SID range (exact
 * SID = {s,s}); we only ever consider mask==0
 * streams (qkx_stream_resolve refuses a non-zero SMR mask), so a candidate
 * matches iff its SID falls in a range. */
struct qkx_sid_range { u16 lo, hi; };
static const struct qkx_sid_range qkx_sid_denylist[] = {
	/* Secure content / CP */
	{ 0x821, 0x821 },			/* display secure (SMR mask 0x400 also matches 0xc21, which is in the camera range below) */
	{ 0x2101, 0x2101 }, { 0x2103, 0x2104 },	/* Venus secure */
	{ 0x2123, 0x2124 },			/* CVP secure */
	{ 0x584, 0x59f },			/* QCEDEV crypto + adjacent fastrpc */
	/* Firmware-owned windows */
	{ 0x1000, 0x13ff },			/* NPU + CDSP cb1-8 (incl. 0x1009) */
	{ 0x1800, 0x1bff },			/* ADSP audio-ION / fastrpc / USB-audio (0x1801/0x1803-5/0x180f) */
	{ 0x800, 0xcff },			/* camera */
	{ 0x2000, 0x24ff },			/* camera CP / secure */
	{ 0xe0, 0xe0 }, { 0x4e0, 0x4e0 },	/* UFS */
	{ 0x541, 0x543 },			/* firmware-owned */
	/* NB: deliberately excludes USB SID 0x0 and the QUP2 SE-DMA SID 0x63
	 * (syncboss target) — both below the lowest range (0xe0). */
};

static bool qkx_sid_denied(u16 sid)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(qkx_sid_denylist); i++)
		if (sid >= qkx_sid_denylist[i].lo && sid <= qkx_sid_denylist[i].hi)
			return true;
	return false;
}

/* A bypass target named in bypass_devices. `lookup` points into the caller's
 * copy of the param string (alive for the whole operation). Resolve fills
 * sid/idx/cbndx/cb_phys; apply records orig_sctlr and sets applied so an
 * all-or-nothing undo can restore it exactly. There is no built-in allowlist:
 * any platform device name is accepted and the runtime guards (single stream,
 * SMR mask==0, secure-SID denylist, CB-exclusivity, geometry, already-bypassed
 * abort, readback+restore) are the backstop. The caller vets each target. */
#define QKX_BYPASS_MAX 8
struct qkx_bypass_stream {
	const char *lookup;
	u32 sid, cbndx;
	int idx;
	phys_addr_t cb_phys;
	u32 orig_sctlr;
	bool applied;
};

/* find_dev signature (bus_find_device_by_name), passed down from qkx_execute. */
typedef struct device *(*qkx_find_dev_fn)(struct bus_type *, struct device *,
					  const char *);

/* Resolve one bypass target, read-only: find the named platform device, walk up
 * to its SMMU master, run every guard, and compute its CB register address.
 * Fills s->{sid,idx,cbndx,cb_phys}. The method keeps the S2CR TRANSLATE and (in
 * apply) clears only SCTLR.M on the CB — the driver's own S1_BYPASS mode, proven
 * live on the display CB and USB v4b. Only non-secure apps_smmu context banks are
 * ever touched. There is no expected-name/SID allowlist: whatever single stream
 * the named device resolves to is taken, and the guards are the backstop. */
static int qkx_stream_resolve(struct qkx_bypass_stream *s, void __iomem *gr0,
			      u32 numsmrg, const void *apps_smmu,
			      struct bus_type *platform_bus, qkx_find_dev_fn find_dev)
{
	const char *name = s->lookup;
	struct device *ldev, *dev;
	struct iommu_fwspec *fwspec = NULL;
	struct qkx_smmu_master_cfg *cfg;
	u32 sid, smr, smr_id, smr_mask, s2cr, type, id1, pgshift, numcb, cbndx;
	u32 n, cb_users;
	unsigned long cb_off;
	int idx, hops, ret = -EINVAL;

	ldev = find_dev(platform_bus, NULL, name);
	if (!ldev) {
		pr_emerg("quest_kexec: bypass: device %s not found; skipping\n", name);
		return -ENODEV;
	}
	/* The iommus binding is on the master (USB: the ssusb glue; a QUP SE: its
	 * wrapper), a parent of the looked-up device, so walk up a few levels. */
	for (dev = ldev, hops = 0; dev && hops < 4; dev = dev->parent, hops++) {
		if (dev->iommu_fwspec && dev->iommu_fwspec->num_ids >= 1 &&
		    dev->iommu_fwspec->iommu_priv) {
			fwspec = dev->iommu_fwspec;
			break;
		}
	}
	if (!fwspec) {
		pr_emerg("quest_kexec: bypass: no iommu_fwspec on %s or parents; skipping\n",
			 name);
		goto out;
	}
	/* Require exactly one stream so we flip one well-defined CB. Take whatever
	 * SID the device resolves to (no expected-name/SID allowlist); the guards
	 * below are the backstop and the caller vets the target. */
	if (fwspec->num_ids != 1) {
		pr_emerg("quest_kexec: bypass: %s master %s has num_ids=%u (want 1); refusing\n",
			 name, dev_name(dev), fwspec->num_ids);
		goto out;
	}
	sid = fwspec->ids[0];
	cfg = fwspec->iommu_priv;
	/* The CB writes are hard-wired to apps_smmu (0x15000000). Refuse
	 * unless this master is actually on apps_smmu — compare its arm_smmu_device
	 * (fwspec->iommu_priv->smmu) against apps_smmu's driver data. This
	 * structurally excludes the GPU SMMU and any other instance. */
	if (!apps_smmu || cfg->smmu != apps_smmu) {
		pr_emerg("quest_kexec: bypass: %s master %s is not on apps_smmu; refusing\n",
			 name, dev_name(dev));
		goto out;
	}
	idx = cfg->smendx[0];
	if (idx < 0 || (u32)idx >= numsmrg) {
		pr_emerg("quest_kexec: bypass: %s idx=%d out of range (numsmrg=%u); refusing\n",
			 name, idx, numsmrg);
		goto out;
	}
	/* SMR must be valid, exact-match (mask==0) and carry this SID. A non-zero
	 * mask could claim other SIDs' transactions (including a denylisted secure
	 * SID), so refuse it. */
	smr = readl_relaxed(gr0 + QKX_SMMU_GR0_SMR(idx));
	smr_id = (smr >> QKX_SMMU_SMR_ID_SHIFT) & QKX_SMMU_SMR_FIELD;
	smr_mask = (smr >> QKX_SMMU_SMR_MASK_SHIFT) & QKX_SMMU_SMR_FIELD;
	if (!(smr & QKX_SMMU_SMR_VALID) || smr_mask != 0 || smr_id != sid) {
		pr_emerg("quest_kexec: bypass: %s SMR[%d]=%#x (valid=%d id=%#x mask=%#x) not exact SID %#x; refusing\n",
			 name, idx, smr, !!(smr & QKX_SMMU_SMR_VALID),
			 smr_id, smr_mask, sid);
		goto out;
	}
	/* Secure / firmware-owned SID denylist backstop (mask==0, so an exact-SID
	 * test). This blocks a mistaken entry from flipping a secure CB. */
	if (qkx_sid_denied(sid)) {
		pr_emerg("quest_kexec: bypass: %s SID %#x is on the secure/off-limits denylist; refusing\n",
			 name, sid);
		goto out;
	}
	/* S2CR must translate; take its CB index. */
	s2cr = readl_relaxed(gr0 + QKX_SMMU_GR0_S2CR(idx));
	type = (s2cr >> QKX_S2CR_TYPE_SHIFT) & QKX_S2CR_TYPE_MASK;
	if (type != QKX_S2CR_TYPE_TRANS) {
		pr_emerg("quest_kexec: bypass: %s S2CR %#x TYPE=%u not TRANSLATE; refusing\n",
			 name, s2cr, type);
		goto out;
	}
	cbndx = (s2cr >> QKX_S2CR_CBNDX_SHIFT) & QKX_S2CR_CBNDX_MASK;
	/* CB-exclusivity: exactly one TRANSLATE S2CR may target this CB, else
	 * bypassing it would also bypass an unrelated stream. NOTE: this catches
	 * several SIDs sharing a CB; it does NOT catch one wrapper SID shared by
	 * several devices (e.g. a QUP SE-DMA wrapper) — that is a run-plan pre-flight. */
	cb_users = 0;
	for (n = 0; n < numsmrg; n++) {
		u32 r = readl_relaxed(gr0 + QKX_SMMU_GR0_S2CR(n));

		if (((r >> QKX_S2CR_TYPE_SHIFT) & QKX_S2CR_TYPE_MASK) ==
			QKX_S2CR_TYPE_TRANS &&
		    ((r >> QKX_S2CR_CBNDX_SHIFT) & QKX_S2CR_CBNDX_MASK) == cbndx)
			cb_users++;
	}
	if (cb_users != 1) {
		pr_emerg("quest_kexec: bypass: %s CB%u shared by %u TRANS streams (want 1); refusing\n",
			 name, cbndx, cb_users);
		goto out;
	}
	/* CB register geometry from ID1 (same as arm-smmu.c): require 4 KB pages so
	 * CB(cbndx) stays inside the mapped 1 MB window, cbndx within the implemented
	 * banks, and the whole CB register page inside the window. */
	id1 = readl_relaxed(gr0 + QKX_SMMU_GR0_ID1);
	pgshift = (id1 & QKX_SMMU_ID1_PAGESIZE) ? 16 : 12;
	numcb = (id1 >> QKX_SMMU_ID1_NUMCB_SHIFT) & QKX_SMMU_ID1_NUMCB_MASK;
	cb_off = QKX_APPS_SMMU_CB_BASE_OFF + ((unsigned long)cbndx << pgshift);
	if (pgshift != 12 || cbndx >= numcb || cb_off + SZ_4K > QKX_APPS_SMMU_SIZE) {
		pr_emerg("quest_kexec: bypass: %s cbndx=%u numcb=%u pgshift=%u off=%#lx geometry refuse\n",
			 name, cbndx, numcb, pgshift, cb_off);
		goto out;
	}
	s->sid = sid;
	s->idx = idx;
	s->cbndx = cbndx;
	s->cb_phys = QKX_APPS_SMMU_PHYS + cb_off;
	pr_emerg("quest_kexec: bypass: resolved %s -> master %s sid=%#x idx=%d CB%u @%pa\n",
		 name, dev_name(dev), sid, idx, cbndx, &s->cb_phys);
	ret = 0;
out:
	put_device(ldev);
	return ret;
}

/* Apply one resolved stream: flip SCTLR.M to bypass and verify, recording the
 * original so a later all-or-nothing undo can restore it. */
static int qkx_stream_apply(struct qkx_bypass_stream *s)
{
	void __iomem *cb;
	u32 orig, want, back, fsr;
	int delay;

	cb = ioremap(s->cb_phys, SZ_4K);
	if (!cb)
		return -ENOMEM;
	/* A translating (atomic) domain has M=1 here; M=0 means a RAZ read
	 * (invisible/unpowered CB) or the wrong CB — abort rather than mistranslate. */
	orig = readl_relaxed(cb + QKX_SMMU_CB_SCTLR);
	if (!(orig & QKX_SMMU_SCTLR_M)) {
		pr_emerg("quest_kexec: bypass: %s CB%u SCTLR=%#x unexpected M=0 (RAZ or wrong CB); aborting\n",
			 s->lookup, s->cbndx, orig);
		iounmap(cb);
		return -EIO;
	}
	/* Terminate a stalled fault transaction (FSR.SS) and clear latched faults. */
	fsr = readl_relaxed(cb + QKX_SMMU_CB_FSR);
	if (fsr & QKX_SMMU_FSR_SS) {
		writel_relaxed(QKX_SMMU_RESUME_TERMINATE, cb + QKX_SMMU_CB_RESUME);
		mb();
	}
	if (fsr) {
		writel_relaxed(fsr, cb + QKX_SMMU_CB_FSR);
		mb();
		pr_emerg("quest_kexec: bypass: %s CB%u cleared FSR=%#x%s\n",
			 s->lookup, s->cbndx, fsr,
			 (fsr & QKX_SMMU_FSR_SS) ? " (SS: terminated)" : "");
	}
	/* Clear only SCTLR.M -> stage-1 bypass, preserving every other SCTLR bit. */
	want = orig & ~QKX_SMMU_SCTLR_M;
	writel_relaxed(want, cb + QKX_SMMU_CB_SCTLR);
	mb();
	/* CB-scoped stage-1 TLB invalidate + sync (never a global TLBI). */
	writel_relaxed(0, cb + QKX_SMMU_CB_S1_TLBIALL);
	mb();
	writel_relaxed(QKX_SMMU_SYNC_VAL, cb + QKX_SMMU_CB_TLBSYNC);
	for (delay = 0; delay < QKX_SMMU_TLBSYNC_TIMEOUT_US; delay++) {
		if (!(readl_relaxed(cb + QKX_SMMU_CB_TLBSTATUS) &
		      QKX_SMMU_TLBSTATUS_SACTIVE))
			break;
		udelay(1);
	}
	if (delay >= QKX_SMMU_TLBSYNC_TIMEOUT_US)
		pr_emerg("quest_kexec: bypass: %s CB%u TLBSYNC timeout after %dus (continuing)\n",
			 s->lookup, s->cbndx, delay);
	/* Readback-verify; on an unexpected trap restore the original and fail so the
	 * caller unwinds the whole set. */
	back = readl_relaxed(cb + QKX_SMMU_CB_SCTLR);
	if (back & QKX_SMMU_SCTLR_M) {
		writel_relaxed(orig, cb + QKX_SMMU_CB_SCTLR);
		mb();
		iounmap(cb);
		pr_emerg("quest_kexec: bypass: %s CB%u SCTLR readback %#x still M=1 (rejected?); restored %#x\n",
			 s->lookup, s->cbndx, back, orig);
		return -EIO;
	}
	s->orig_sctlr = orig;
	s->applied = true;
	iounmap(cb);
	pr_emerg("quest_kexec: bypass: %s CB%u SCTLR %#x -> %#x (stage-1 bypass, TLBSYNC %dus)\n",
		 s->lookup, s->cbndx, orig, back, delay);
	return 0;
}

/* Undo one applied stream (restore the recorded SCTLR), for an all-or-nothing
 * unwind when a later stream rejects. Returns 0 on verified restore, -errno if
 * the CB could not be put back: the caller must then treat the jump as
 * unsafe, because Android would be left with a bypassed CB. */
static int qkx_stream_undo(struct qkx_bypass_stream *s)
{
	void __iomem *cb;
	u32 back;

	if (!s->applied)
		return 0;
	cb = ioremap(s->cb_phys, SZ_4K);
	if (!cb) {
		pr_emerg("quest_kexec: bypass: %s CB%u undo ioremap FAILED; CB left bypassed (M=0)!\n",
			 s->lookup, s->cbndx);
		return -ENOMEM;
	}
	writel_relaxed(s->orig_sctlr, cb + QKX_SMMU_CB_SCTLR);
	mb();
	/* Read back and confirm the restore took (M set again). */
	back = readl_relaxed(cb + QKX_SMMU_CB_SCTLR);
	iounmap(cb);
	if (back != s->orig_sctlr) {
		pr_emerg("quest_kexec: bypass: %s CB%u undo readback %#x != %#x; CB may be left bypassed!\n",
			 s->lookup, s->cbndx, back, s->orig_sctlr);
		return -EIO;
	}
	s->applied = false;
	pr_emerg("quest_kexec: bypass: %s CB%u SCTLR restored to %#x (undo)\n",
		 s->lookup, s->cbndx, s->orig_sctlr);
	return 0;
}

/* Resolve every configured stream read-only, then apply them all-or-nothing:
 * any resolve failure aborts before any write; any apply failure undoes the
 * streams already flipped. */
static int qkx_bypass_all(struct qkx_bypass_stream *streams, int n,
			  struct bus_type *platform_bus, qkx_find_dev_fn find_dev,
			  bool dry)
{
	void __iomem *gr0;
	const void *apps_smmu;
	struct device *smmu_dev;
	u32 numsmrg;
	int i, j, ret;

	/* The apps_smmu arm_smmu_device, for the per-stream on-apps_smmu
	 * check. Pointer-compare only (never dereferenced), so dropping the device
	 * ref here is safe while the driver stays bound (it does, we are frozen). */
	smmu_dev = find_dev(platform_bus, NULL, QKX_APPS_SMMU_DEVNAME);
	apps_smmu = smmu_dev ? dev_get_drvdata(smmu_dev) : NULL;
	put_device(smmu_dev);
	if (!apps_smmu) {
		pr_emerg("quest_kexec: bypass: apps_smmu (%s) not found/bound; refusing all\n",
			 QKX_APPS_SMMU_DEVNAME);
		return -ENODEV;
	}
	gr0 = ioremap(QKX_APPS_SMMU_PHYS, SZ_4K);
	if (!gr0)
		return -ENOMEM;
	numsmrg = readl_relaxed(gr0 + QKX_SMMU_GR0_ID0) & QKX_SMMU_ID0_NUMSMRG;
	for (i = 0; i < n; i++) {
		ret = qkx_stream_resolve(&streams[i], gr0, numsmrg, apps_smmu,
					 platform_bus, find_dev);
		if (ret)
			goto out;
	}
	/* Two entries must not resolve to the same CB (the second apply would see
	 * M=0 and abort anyway, but reject up front). */
	for (i = 0; i < n; i++)
		for (j = i + 1; j < n; j++)
			if (streams[i].cbndx == streams[j].cbndx) {
				pr_emerg("quest_kexec: bypass: entries %d and %d share CB%u; refusing\n",
					 i, j, streams[i].cbndx);
				ret = -EINVAL;
				goto out;
			}
	if (dry) {
		/* bypass_resolve_only: every stream resolved read-only and passed all
		 * guards; stop before any CB write so a Phase-A pre-flight can confirm
		 * the resolution from the log. */
		pr_emerg("quest_kexec: bypass: DRY RUN — %d stream(s) resolved, no CB written, no jump\n",
			 n);
		ret = 0;
		goto out;
	}
	for (i = 0; i < n; i++) {
		ret = qkx_stream_apply(&streams[i]);
		if (ret) {
			int undo_fail = 0;

			while (--i >= 0)
				if (qkx_stream_undo(&streams[i]))
					undo_fail = 1;
			/* Fail-closed: a clean undo restores every flipped CB, so
			 * we can safely abort the jump and return to Android. But if an undo
			 * could NOT put a CB back, Android would resume with a non-secure CB
			 * stuck in IOVA=PA bypass — its own DMA through it would corrupt RAM.
			 * Do not return to Android in that state: panic so the reset re-inits
			 * the SMMU via PBL and clears the stuck bypass. (Only reachable with
			 * >1 entry; a single-entry run self-restores in qkx_stream_apply.) */
			if (undo_fail) {
				iounmap(gr0);
				panic("quest_kexec: bypass undo left a CB bypassed; resetting to restore SMMU state");
			}
			goto out;
		}
	}
	ret = 0;
out:
	iounmap(gr0);
	return ret;
}

/* Parse the bypass_devices list in `buf` (modified in place; each stream's lookup
 * points into it, so buf must outlive the streams) into the streams array.
 * De-dups. Returns the count or -errno. No hardware access, so also usable at
 * init to validate the param.
 *
 * Separator is '/'. It can never appear in a device's dev_name and is not special
 * to any shell, so the value needs no quoting anywhere (run.sh passes it unquoted
 * through `adb shell` to the device shell; a shell-special separator like ';' or
 * whitespace would be split by that shell and SILENTLY half-apply the bypass — the
 * v3 stale-translation hazard with no loader error). As defense in depth we first
 * reject any character outside the safe device-name+'/' set (covers ';', spaces,
 * and every shell metacharacter), so a mis-quoted value fails loudly, not silently. */
static int qkx_parse_bypass(char *buf, struct qkx_bypass_stream *streams, int max)
{
	char *tok, *p = buf;
	int n = 0, i;

	for (i = 0; buf[i]; i++) {
		char c = buf[i];

		if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
		      (c >= 'A' && c <= 'Z') || c == '/' || c == '.' ||
		      c == ',' || c == '-' || c == '_' || c == '@' || c == ':')) {
			pr_emerg("quest_kexec: bypass: bypass_devices has illegal char %#04x (separator is '/', no quoting/whitespace/';'); refusing\n",
				 (unsigned char)c);
			return -EINVAL;
		}
	}
	while ((tok = strsep(&p, "/")) != NULL) {
		tok = strim(tok);
		if (!*tok)
			continue;
		for (i = 0; i < n; i++)
			if (!strcmp(streams[i].lookup, tok))
				break;
		if (i < n)
			continue;		/* de-dup */
		if (n >= max)
			return -E2BIG;
		memset(&streams[n], 0, sizeof(streams[n]));
		streams[n].lookup = tok;
		n++;
	}
	return n;
}

static bool qkx_bypass_configured(void)
{
	return bypass_devices && *bypass_devices;
}

/* Top-level entry. From qkx_execute (dry=false, after the masters are quiesced)
 * it resolves + flips; from qkx_init with bypass_resolve_only (dry=true) it
 * resolves + logs only. Owns the copy of bypass_devices the lookups point into. */
static int qkx_run_bypass(struct bus_type *platform_bus, qkx_find_dev_fn find_dev,
			  bool dry)
{
	struct qkx_bypass_stream streams[QKX_BYPASS_MAX];
	char *copy;
	int n, ret;

	if (!qkx_bypass_configured())
		return 0;
	copy = kstrdup(bypass_devices, GFP_KERNEL);
	if (!copy)
		return -ENOMEM;
	/* streams[].lookup point into `copy`, so it must outlive qkx_bypass_all. */
	n = qkx_parse_bypass(copy, streams, QKX_BYPASS_MAX);
	if (n < 0) {
		kfree(copy);
		return n;		/* malformed / too many */
	}
	if (n == 0) {
		/* A non-empty bypass_devices (e.g. ",") that yields no entries
		 * is a configuration error — refuse rather than silently jump with no
		 * bypass applied. */
		pr_emerg("quest_kexec: bypass: bypass_devices='%s' parsed to 0 entries; refusing\n",
			 bypass_devices);
		kfree(copy);
		return -EINVAL;
	}
	ret = qkx_bypass_all(streams, n, platform_bus, find_dev, dry);
	kfree(copy);
	return ret;
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
	int (*resume_dwc3_gadget)(void *) =
		(void *)lookup("dwc3_gadget_resume");
	int (*rpmh_flush_fn)(const struct device *) =
		(void *)lookup("rpmh_flush");
	/* Optional diagnostic: if the CPU freeze ever aborts (a wake IRQ beating
	 * the negative pm_abort_suspend margin seeded before machine_shutdown),
	 * name the source before panicking. Use pm_get_active_wakeup_sources(),
	 * which fills a buffer via scnprintf, rather than
	 * pm_print_active_wakeup_sources(), which emits through pr_debug and is
	 * silent on a production build. NULL-guarded at the call site,
	 * so a missing symbol can never affect staging or the normal jump path. */
	void (*get_wakeup_sources)(char *, size_t) =
		(void *)lookup("pm_get_active_wakeup_sources");
	struct device *gpu, *watchdog, *usb_core = NULL, *rpmh_dev = NULL;
	struct device *syncboss = NULL, *qmp_dev = NULL;
	struct task_struct *wakeup_suppressor = NULL;
	bool syncboss_suspended = false;
	void __iomem *wdt_regs = NULL;
	bool sleep_locked = false;
	bool froze = false;
	bool events_check_was = false;
	bool events_check_cleared = false;
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
	    !suspend_dwc3_gadget || (qkx_bypass_configured() && !resume_dwc3_gadget) ||
	    (suspend_syncboss && !spi_bus))
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
	 * Remember the prior value so an abort restores it exactly rather
	 * than forcing true, and track that we cleared it so the restore runs on
	 * every exit after this point, including a freeze_processes failure (before
	 * froze is set). */
	events_check_was = READ_ONCE(*events_check_enabled);
	events_check_cleared = true;
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
	/* From here userspace is frozen and CPUs may enter deep idle. If we abort
	 * after this point, Android's msm_watchdog petter can miss pets (its
	 * cross-CPU ping stalls on a CPU in that window), and a preserved HW
	 * watchdog would bark-reboot a few seconds later. The release path uses
	 * this flag to pet+disarm the watchdog and re-enable wakeup-event checking
	 * before handing control back to Android. */
	froze = true;
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
	{
		struct dwc3 *dwc = dev_get_drvdata(usb_core);
		int in_lpm = atomic_read(&dwc->in_lpm);

		pr_emerg("quest_kexec: DWC3 in_lpm=%d gadget_driver=%s\n",
			in_lpm, dwc->gadget_driver ? "bound" : "none");
		/* Qualcomm defines in_lpm as controller clocks off. The stock
		 * gadget suspend callback starts with unguarded MMIO, which can
		 * hang the CPU in charger-only/no-host operation. Already-low-power
		 * hardware needs no further gadget stop; preserve its power domain.
		 */
		if (in_lpm) {
			qkx_phase(10, "DWC3 already in low power; skipping gadget MMIO", false);
		} else {
			qkx_phase(10, "suspending Android DWC3 gadget", false);
			ret = suspend_dwc3_gadget(dwc);
			pr_emerg("quest_kexec: DWC3 gadget suspend returned %d\n", ret);
			if (ret)
				panic("quest_kexec: DWC3 gadget suspend failed (%d)", ret);
		}
	}
	/* With the masters quiesced (no in-flight DMA) optionally hand each
	 * configured apps_smmu stream (bypass_devices) to a non-managing target
	 * kernel by clearing its CB's SCTLR.M. All-or-nothing: if any entry cannot
	 * be applied (device not found, guard refusal, or a rejected flip), every CB
	 * already flipped is restored and the jump aborts before the point of no
	 * return — booting the target with a half-applied set would run DMA against a
	 * stale stock translation and corrupt RAM. */
	if (qkx_bypass_configured()) {
		qkx_phase(101, "applying configured apps_smmu identity-DMA bypass set", false);
		ret = qkx_run_bypass(platform_bus, find_dev, false);
		pr_emerg("quest_kexec: SMMU bypass set returned %d\n", ret);
		if (ret) {
			pr_emerg("quest_kexec: SMMU bypass set not fully applied; aborting jump\n");
			/* qkx_run_bypass is all-or-nothing: every CB it flipped is already
			 * restored. Resume the dwc3 gadget we suspended (syncboss is resumed
			 * in release_devices) so Android USB survives, then unwind. */
			resume_dwc3_gadget(dev_get_drvdata(usb_core));
			gpu_resume(gpu);
			thaw_users();
			goto release_devices;
		}
	}
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
	 * wake IRQ during device shutdown would abort it and force a panic.
	 * events_check_enabled=false kills the count-mismatch half of the check;
	 * the other half is pm_abort_suspend>0, which pm_system_wakeup()'s
	 * atomic_inc() can re-arm from a wake IRQ at any point across the per-CPU
	 * freeze loop. A one-shot clear to 0 races that: freeze_secondary_cpus()
	 * takes the secondaries down one at a time, so any IRQ landing mid-loop
	 * trips the next check (the "native CPU shutdown failed" panic). A concurrent
	 * suppressor kthread can't help here the way it does for freeze_processes,
	 * because machine_shutdown() takes the other CPUs offline. Instead seed a
	 * large negative margin (same value qkx_wakeup_suppressor uses): any
	 * realistic number of wake-IRQ increments during the freeze stays <= 0, so
	 * pm_wakeup_pending() keeps returning false. Past this point we either jump
	 * (never return) or panic below, so the margin needs no later cleanup. */
	WRITE_ONCE(*events_check_enabled, false);
	atomic_set(qkx_pm_abort_suspend, -1000000);
	qkx_phase(13, "calling native machine_shutdown", false);
	shutdown_cpus();
	qkx_phase(14, "native machine_shutdown returned", false);
	if (num_online_cpus() != 1 || stuck()) {
		/* Should not happen now the margin is seeded; if it ever does, name
		 * the wake source that beat it so the next failure is diagnosable. */
		pr_emerg("quest_kexec: CPU freeze failed: online=%u pm_abort_suspend=%d events_check_enabled=%d\n",
			 num_online_cpus(), atomic_read(qkx_pm_abort_suspend),
			 READ_ONCE(*events_check_enabled));
		if (get_wakeup_sources) {
			char wsbuf[256] = "";

			get_wakeup_sources(wsbuf, sizeof(wsbuf));
			pr_emerg("quest_kexec: %s\n", wsbuf);
		}
		panic("quest_kexec: native CPU shutdown failed");
	}
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
	/* Restore wakeup-event checking to its prior value on every
	 * exit after we cleared it (including a freeze_processes failure, where froze
	 * is not set), rather than forcing it true or only in the froze path. */
	if (events_check_cleared)
		WRITE_ONCE(*events_check_enabled, events_check_was);
	/* Post-freeze abort recovery (pet-only). We entered the frozen/deep-
	 * idle window, so Android's watchdog petter may have missed pets. PET the
	 * preserved HW apps-watchdog but LEAVE IT ARMED:
	 *   - a transient petter stall gets a fresh ~12 s bark window and recovers,
	 *     the same end state as disarming;
	 *   - a genuine CPU wedge (a CPU that never answered the stop IPI) still
	 *     barks ~12-15 s later, which resets a truly sick
	 *     Android automatically AND leaves a pstore trace — strictly better than
	 *     an indefinite silent hang. The bark dump also names the wedged CPUs, so
	 *     no loader-side ping diagnostic is needed (and a module-resident ping
	 *     would be a use-after-free once the module is freed).
	 * (A successful jump never reaches here; qkx_enter does not return.) */
	if (froze && wdt_regs) {
		writel_relaxed(1, wdt_regs + 0x04);	/* WDT0_RST: reset bark counter */
		mb();
		pr_emerg("quest_kexec: abort unwind: petted preserved apps-watchdog (left armed); EN=%x\n",
			 readl_relaxed(wdt_regs + 0x08));
	}
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

/* The 8 APSS core-hang config registers, and the exact pre-modification
 * values captured by qkx_core_hang_configure so qkx_core_hang_restore writes back
 * precisely what was there rather than a reconstructed value. */
static const phys_addr_t qkx_core_hang_cfg[] = {
	0x18000060, 0x18010060, 0x18020060, 0x18030060,
	0x18040060, 0x18050060, 0x18060060, 0x18070060,
};
static u32 qkx_core_hang_orig[ARRAY_SIZE(qkx_core_hang_cfg)];
static bool qkx_core_hang_saved;

static int qkx_core_hang_configure(void)
{
	u32 (*secure_read)(phys_addr_t) = (void *)lookup("scm_io_read");
	int (*secure_write)(phys_addr_t, u32) = (void *)lookup("scm_io_write");
	unsigned int i;

	if (!secure_read || (core_hang_control == 2 && !secure_write))
		return -ENOENT;
	/* Snapshot every register's exact original BEFORE modifying
	 * any, and publish the snapshot. Then even a mid-loop write failure leaves a
	 * complete, correct set of originals for qkx_core_hang_restore to undo. */
	for (i = 0; i < ARRAY_SIZE(qkx_core_hang_cfg); i++)
		qkx_core_hang_orig[i] = secure_read(qkx_core_hang_cfg[i]);
	qkx_core_hang_saved = true;
	for (i = 0; i < ARRAY_SIZE(qkx_core_hang_cfg); i++) {
		u32 before = qkx_core_hang_orig[i];
		int ret = 0;
		u32 after = before;

		if (core_hang_control == 2 && (before & BIT(1))) {
			ret = secure_write(qkx_core_hang_cfg[i], before & ~BIT(1));
			after = secure_read(qkx_core_hang_cfg[i]);
		}
		pr_emerg("quest_kexec: core-hang cpu%u config=%pa before=%08x after=%08x write=%d\n",
			i, &qkx_core_hang_cfg[i], before, after, ret);
		if (ret || (core_hang_control == 2 && (after & BIT(1))))
			return ret ? ret : -EIO;
	}
	return 0;
}

/* Re-arm the APSS core-hang detection that core_hang_control==2 disabled, so a
 * run that hands control back to Android (any abort or a preflight/rehearsal
 * return) leaves core-hang as we found it. A successful jump never returns here,
 * so the target still boots with it disabled, as intended. This does not by
 * itself fix the watchdog bark (that is the HW apps-watchdog, handled in the
 * execute unwind) — it is state hygiene. Restore the exact captured original
 * per register, only where we changed it, rather than reconstructing the value. */
static void qkx_core_hang_restore(void)
{
	int (*secure_write)(phys_addr_t, u32) = (void *)lookup("scm_io_write");
	u32 (*secure_read)(phys_addr_t) = (void *)lookup("scm_io_read");
	unsigned int i;

	if (!qkx_core_hang_saved || !secure_write)
		return;
	for (i = 0; i < ARRAY_SIZE(qkx_core_hang_cfg); i++) {
		u32 now = secure_read ? secure_read(qkx_core_hang_cfg[i]) : 0;
		int ret;

		if (now == qkx_core_hang_orig[i])
			continue;	/* unchanged (or already restored) */
		ret = secure_write(qkx_core_hang_cfg[i], qkx_core_hang_orig[i]);
		pr_emerg("quest_kexec: core-hang restore cpu%u %08x->%08x ret=%d\n",
			 i, now, qkx_core_hang_orig[i], ret);
	}
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
	    (watchdog_control && !watchdog_recovery) ||
	    (bypass_devices && *bypass_devices && !execute && !bypass_resolve_only) ||
	    (bypass_resolve_only && execute) ||
	    (bypass_resolve_only && !qkx_bypass_configured()))
		return -EINVAL;
	if ((execute && (test_transition || test_cpus)) || (test_transition && test_cpus))
		return -EINVAL;
	/* Parse-validate bypass_devices up front (count/dedup) so a malformed or
	 * oversized list fails at insmod; the device resolution + guards run at
	 * execute. */
	if (qkx_bypass_configured()) {
		struct qkx_bypass_stream tmp[QKX_BYPASS_MAX];
		char *copy = kstrdup(bypass_devices, GFP_KERNEL);

		if (!copy)
			return -ENOMEM;
		ret = qkx_parse_bypass(copy, tmp, QKX_BYPASS_MAX);
		kfree(copy);
		if (ret < 0)
			return ret;
		if (ret == 0)	/* non-empty but no valid entries */
			return -EINVAL;
	}
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
	if (bypass_resolve_only) {
		/* Dry run: resolve every bypass_devices entry read-only and log it, with
		 * no CB write, no staging and no jump. insmod success = all resolved
		 * (read dmesg, then rmmod); failure = a stream did not resolve. */
		struct bus_type *platform_bus = (void *)lookup("platform_bus_type");
		qkx_find_dev_fn find_dev = (void *)lookup("bus_find_device_by_name");

		if (!platform_bus || !find_dev)
			return -ENOENT;
		ret = qkx_run_bypass(platform_bus, find_dev, true);
		pr_emerg("quest_kexec: bypass_resolve_only dry run returned %d; no CB write, no jump\n",
			 ret);
		return ret;
	}
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
		if (ret) {
			/* A partial configure still hands control back to Android,
			 * so undo whatever it changed before returning. */
			if (core_hang_control == 2)
				qkx_core_hang_restore();
			return ret;
		}
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
	if (memcmp(kh + 56, "ARM\x64", 4) || get_unaligned_le64(kh + 8) != kernel_text_offset ||
	    kernel_text_offset >= QKX_INITRD - QKX_LO ||
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
	/* Reaching here means we are returning to Android (a successful jump never
	 * returns). If core_hang_control==2 disabled core-hang detection above,
	 * re-arm it so Android is handed back the state it started with. */
	if (core_hang_control == 2)
		qkx_core_hang_restore();
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
MODULE_DESCRIPTION("Experimental Meta Quest (Kona) ARM64 kexec staging and EL1 transition");

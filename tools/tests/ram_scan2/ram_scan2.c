// SPDX-License-Identifier: GPL-2.0
/*
 * Read-only probe of every linear-mapped System RAM page. Prints a marker
 * before each 16 MiB chunk so the last marker streamed out identifies a
 * range whose access hangs the CPU. Nothing is written.
 */
#include <linux/module.h>
#include <linux/mm.h>
#include <linux/ioport.h>
#include <linux/delay.h>

static unsigned long start_pfn;
static unsigned long read_pages, reserved_pages, invalid_pages, busy_pages;
module_param(start_pfn, ulong, 0444);
static unsigned long end_pfn = PFN_DOWN(0x380000000ULL);
module_param(end_pfn, ulong, 0444);

static int scan_range(unsigned long pfn, unsigned long nr, void *arg)
{
	unsigned long end = min(pfn + nr, end_pfn), *sum = arg;

	if (pfn < start_pfn)
		pfn = min(start_pfn, end);
	for (; pfn < end; pfn++) {
		if (!(pfn & 0xff)) {
			pr_info("ramscan2: at %#llx\n", (u64)PFN_PHYS(pfn));
			msleep(1);
		}
		if (!pfn_valid(pfn)) {
			invalid_pages++;
			continue;
		}
		/* pfn_valid means a linear map exists, not that HLOS owns the
		 * page. Never probe firmware/display/kernel-reserved carveouts. */
		if (PageReserved(pfn_to_page(pfn))) {
			reserved_pages++;
			continue;
		}
		/* An allocated page can belong to a secure GPU/ION VM even
		 * though its PFN is valid. page_count includes compound heads. */
		if (page_count(pfn_to_page(pfn))) {
			busy_pages++;
			continue;
		}
		*sum += READ_ONCE(*(unsigned long *)page_address(pfn_to_page(pfn)));
		read_pages++;
	}
	return 0;
}

static int __init ram_scan_init(void)
{
	unsigned long sum = 0;

	pr_info("ramscan2: begin from pfn %#lx\n", start_pfn);
	scan_range(PFN_DOWN(0x80000000ULL), PFN_DOWN(0x300000000ULL), &sum);
	pr_info("ramscan2: done sum=%lx read=%lu MiB reserved=%lu MiB unmapped=%lu MiB busy=%lu MiB\n",
		sum, read_pages >> 8, reserved_pages >> 8, invalid_pages >> 8,
		busy_pages >> 8);
	return -EAGAIN;
}
module_init(ram_scan_init);
MODULE_LICENSE("GPL");

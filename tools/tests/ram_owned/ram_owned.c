// SPDX-License-Identifier: GPL-2.0
/* Destructive ONLY to pages allocated by this module through the buddy allocator.
 * Never maps arbitrary physical addresses or touches reserved firmware memory. */
#include <linux/module.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <asm/cacheflush.h>

static unsigned int mib = 10240;
module_param(mib, uint, 0444);
#define ORDER 8
#define BYTES (PAGE_SIZE << ORDER)

static int __init ram_owned_init(void)
{
 struct page **pages;
 unsigned int i, n = 0;
 unsigned long j, mismatches = 0;
 phys_addr_t lo = ~(phys_addr_t)0, hi = 0, pa;
 unsigned long *p, expect;
 if (!mib || mib > 12288)
  return -EINVAL;
 pages = kcalloc(mib, sizeof(*pages), GFP_KERNEL);
 if (!pages)
  return -ENOMEM;
 for (n = 0; n < mib; n++) {
  pages[n] = alloc_pages(GFP_HIGHUSER_MOVABLE | __GFP_NORETRY | __GFP_NOWARN, ORDER);
  if (!pages[n])
   break;
  pa = page_to_phys(pages[n]);
  lo = min(lo, pa);
  hi = max(hi, pa + BYTES);
  if (!(n & 255)) {
   pr_info("ramowned: allocated %u MiB last=%pa\n", n + 1, &pa);
   cond_resched();
  }
 }
 pr_info("ramowned: allocation complete %u/%u MiB physical span %pa-%pa\n", n, mib, &lo, &hi);
 for (i = 0; i < n; i++) {
  p = page_address(pages[i]);
  pa = page_to_phys(pages[i]);
  for (j = 0; j < BYTES / sizeof(*p); j++)
   WRITE_ONCE(p[j], (unsigned long)(pa + j * sizeof(*p)) ^ 0xa55aa55a5aa55aa5UL);
  __flush_dcache_area(p, BYTES);
  if (!(i & 255)) {
   pr_info("ramowned: filled %u MiB last=%pa\n", i + 1, &pa);
   cond_resched();
  }
 }
 /* Fill every block before reading any: address aliases between distant
  * banks must not pass by being written and checked one block at a time. */
 mb();
 for (i = 0; i < n; i++) {
  p = page_address(pages[i]);
  pa = page_to_phys(pages[i]);
  for (j = 0; j < BYTES / sizeof(*p); j++) {
   expect = (unsigned long)(pa + j * sizeof(*p)) ^ 0xa55aa55a5aa55aa5UL;
   if (READ_ONCE(p[j]) != expect)
    mismatches++;
  }
  if (!(i & 255)) {
   pr_info("ramowned: verified %u MiB last=%pa mismatches=%lu\n", i + 1, &pa, mismatches);
   cond_resched();
  }
 }
 for (i = 0; i < n; i++)
  __free_pages(pages[i], ORDER);
 kfree(pages);
 pr_info("ramowned: DONE tested=%u MiB requested=%u mismatches=%lu; all pages freed\n", n, mib, mismatches);
 return -EAGAIN;
}
module_init(ram_owned_init);
MODULE_LICENSE("GPL");

/* Read-only snapshot of stock master-side secure SMMU table pages.
 * Layout copied from the matching stock arm-smmu.c; public VMID/TTBR attrs
 * corroborate it before traversal. HLOS has RW but not EXEC on these pages. */
#include <linux/iommu.h>
#include <linux/io-pgtable.h>
#include <linux/platform_device.h>
#include "kgsl_iommu.h"
struct secmap_smmu_cfg {
 u8 cbndx, irptndx; u16 asid; u32 cbar, procid; int fmt;
 u16 min_asid, max_asid;
};
struct secmap_smmu_domain {
 void *smmu; struct device *dev; struct io_pgtable_ops *pgtbl_ops[2];
 const struct iommu_gather_ops *tlb_ops; struct secmap_smmu_cfg cfg;
 int stage; struct mutex init_mutex; spinlock_t cb_lock, sync_lock;
 struct io_pgtable_cfg pgtbl_cfg[2]; u32 attributes;
 bool slave_side_secure; u32 secure_vmid;
 struct list_head pte_info_list, unassign_list; struct mutex assign_lock;
 struct list_head secure_pool_list, nonsecure_pool; void *logger;
 struct iommu_domain domain;
};
struct secmap_pool_chunk { void *virt; size_t size; struct list_head list; };
static unsigned long smmu_tables;
static struct iommu_domain *seen_domains[256];
static unsigned int seen_count;

static bool secmap_ram(phys_addr_t pa)
{
 return pa >= 0x80000000ULL && pa < 0x380000000ULL && pfn_valid(PHYS_PFN(pa));
}
static void secmap_walk(phys_addr_t pa, unsigned int level, unsigned int entries)
{
 u64 *pte;
 unsigned int i;
 if (!secmap_ram(pa) || level > 3 || entries > 512) return;
 emit(pa & PAGE_MASK, PAGE_SIZE);
 smmu_tables++;
 if (level == 3) return;
 pte = phys_to_virt(pa);
 for (i = 0; i < entries; i++) {
  u64 x = READ_ONCE(pte[i]);
  if ((x & 3) == 3)
   secmap_walk(x & GENMASK_ULL(47, 12), level + 1, 512);
 }
}
static void secmap_domain(struct iommu_domain *domain, const char *name)
{
 struct secmap_smmu_domain *d;
 struct secmap_pool_chunk *chunk;
 int vmid, ret;
 u64 ttbr;
 unsigned int i, levels, entries;
 unsigned long flags;
 if (!domain || seen_count == ARRAY_SIZE(seen_domains)) return;
 for (i = 0; i < seen_count; i++) if (seen_domains[i] == domain) return;
 ret = iommu_domain_get_attr(domain, DOMAIN_ATTR_SECURE_VMID, &vmid);
 if (ret || vmid <= 0 || vmid >= 64) return;
 ret = iommu_domain_get_attr(domain, DOMAIN_ATTR_TTBR0, &ttbr);
 if (ret || !ttbr) return;
 d = container_of(domain, struct secmap_smmu_domain, domain);
 if (d->secure_vmid != vmid || d->slave_side_secure || d->stage != 0 ||
     (ttbr & GENMASK_ULL(47, 12)) !=
     (d->pgtbl_cfg[0].arm_lpae_s1_cfg.ttbr[0] & GENMASK_ULL(47, 12))) {
  pr_info("ionsec: smmu layout/type rejected %s vmid=%d\n", name, vmid);
  return;
 }
 seen_domains[seen_count++] = domain;
 pr_info("ionsec: smmu %s vmid=%d ias=%u oas=%u\n", name, vmid,
         d->pgtbl_cfg[0].ias, d->pgtbl_cfg[0].oas);
 mutex_lock(&d->assign_lock);
 spin_lock_irqsave(&d->cb_lock, flags);
 for (i = 0; i < 2; i++) {
  struct io_pgtable_cfg *c = &d->pgtbl_cfg[i];
  if (!d->pgtbl_ops[i] || c->ias < 25 || c->ias > 48 ||
      !(c->pgsize_bitmap & PAGE_SIZE)) continue;
  levels = DIV_ROUND_UP(c->ias - 12, 9);
  entries = 1U << (c->ias - 12 - 9 * (levels - 1));
  if (c->arm_lpae_s1_cfg.ttbr[0])
   secmap_walk(c->arm_lpae_s1_cfg.ttbr[0] & GENMASK_ULL(47, 12), 4 - levels, entries);
  if (c->arm_lpae_s1_cfg.ttbr[1])
   secmap_walk(c->arm_lpae_s1_cfg.ttbr[1] & GENMASK_ULL(47, 12), 4 - levels, entries);
 }
 list_for_each_entry(chunk, &d->secure_pool_list, list)
  if (secmap_ram(virt_to_phys(chunk->virt))) emit(virt_to_phys(chunk->virt), chunk->size);
 spin_unlock_irqrestore(&d->cb_lock, flags);
 mutex_unlock(&d->assign_lock);
}
static int secmap_device(struct device *dev, void *unused)
{
 secmap_domain(iommu_get_domain_for_dev(dev), dev_name(dev));
 return 0;
}
static void dump_smmu_tables(void)
{
 if (kgsl_drv && kgsl_drv->devp[0]) {
  struct kgsl_pagetable *pt = kgsl_drv->devp[0]->mmu.securepagetable;
  if (pt && pt->priv) {
   struct kgsl_iommu_pt *ipt = pt->priv;
   secmap_domain(ipt->domain, "kgsl-secure");
  }
 }
 bus_for_each_dev(&platform_bus_type, NULL, NULL, secmap_device);
 pr_info("ionsec: smmu table pages=%lu domains=%u\n", smmu_tables, seen_count);
}

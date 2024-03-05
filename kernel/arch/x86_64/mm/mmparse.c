#include <common/types.h>
#include <common/kprint.h>
#include <common/macro.h>
#include <common/size.h>
#include <mm/mm.h>
#include <mm/nvm.h>
#include <mm/numa.h>
#include <drivers/cxl.h>
#include <arch/mmu.h>
#include <arch/drivers/multiboot2.h>

#ifdef DSM_ENABLED
#include <dsm/dsm-single.h>
#endif

extern paddr_t physmem_map[N_PHYS_MEM_POOLS][2];
extern int physmem_map_num;

#ifdef USE_NVM
extern struct nvm_region nvm_region_head[8];
extern int nvm_region_num;

extern paddr_t nvmmem_map[N_PHYS_MEM_POOLS][2];
extern int nvmmem_map_num;
#endif /* USE_NVM */

#ifdef USE_CXL_MEM
extern paddr_t cxlmem_map[N_PHYS_MEM_POOLS][2];
extern int cxlmem_map_num;
#endif /* USE_CXL_MEM */

/* vaddr: kernel image end */
extern char img_end[];

/* Direct mapping*/
extern char CHCORE_PUD_Direct_Mapping[];
extern char CHCORE_PUD_CODE_Mapping[];

#define PRESENT  (1 << 0)
#define WRITABLE (1 << 1)
#define HUGE_1G  (1 << 7)
#define GLOBAL   (1 << 8)
#define NX       (1UL << 63)

// currently, kernel page table can not map address above KBASE
#define MAX_KERNEL_PG_PADDR (~KBASE)

static void refill_kernel_page_table(u64 mem_size)
{
        u64 *direct_mapping;
        u64 idx;

        /*
         * We do not remove the booting mapping in the boot page table
         * because it will not be copied to apps' page tables.
         * Besides, the boot page table will not be used after booting.
         */

        /* Re-setup the direct mapping for all the physical memory */
        direct_mapping = (u64 *)CHCORE_PUD_Direct_Mapping;

        /*
         * Since we bindly mapping 0-4G in header.S,
         * here clear it first while leave the first 1GB.
         */
        for (idx = 1; idx < 4; ++idx)
                *(direct_mapping + idx) = 0;

        /*
         * TODO: Besides the available physical memory,
         * some other memory like PCI device memory should also be mapped.
         * Now, we simply map the maximum available physical memory.
         */

        /* We map the available physical memory here. 0~1G has been mapped. */
        idx = 0;
        while (mem_size > SIZE_1G && idx < 511 /* 0, 1 to 511 */) {
                direct_mapping += 1;
                idx += 1;

                /* Add mapping for 1G, 2G, 3G ... */
                *direct_mapping = (idx << 30) + PRESENT + WRITABLE + HUGE_1G
                                  + GLOBAL + NX;
                kdebug("set direct mapping (%p) to idx %d\n",
                      direct_mapping,
                      idx);
                mem_size -= SIZE_1G;
        }

        idx += 1;
        /* Re-setup the direct mapping for all the physical memory */
        direct_mapping = (u64 *)CHCORE_PUD_CODE_Mapping;
        while (mem_size > SIZE_1G) {
                /* Can not map to the last 1GB, mapped tyo kernel code */
                BUG_ON(idx == 1023);
                /* Add mapping for 1G, 2G, 3G ... */
                *direct_mapping = (idx << 30) + PRESENT + WRITABLE + HUGE_1G
                                  + GLOBAL + NX;
                mem_size -= SIZE_1G;
                direct_mapping += 1;
                idx += 1;
        }

        /* Flush TLB: SMP is not enabled for now. */
        extern void flush_boot_tlb(void);
        flush_boot_tlb();
}

/**
 * remap_memory -- remap a mem region from old place to new place in kernel page table
 * @from_addr: addr of the old place
 * @to_addr: addr of the new place
 * @mem_size: size of the mem resource
 * return mapped kernel vaddr
 */
void remap_memory(u64 from_addr, u64 to_addr, u64 mem_size)
{
        u64 *new_mapping, *old_mapping;
        u64 from_idx, to_idx;

        BUG_ON(!IS_ALIGNED(to_addr, SIZE_1G));
        to_idx = to_addr / SIZE_1G;

        BUG_ON(!IS_ALIGNED(from_addr, SIZE_1G));
        from_idx = from_addr / SIZE_1G;

        /* Re-setup the direct mapping for all the physical memory */
        new_mapping = (u64 *)CHCORE_PUD_Direct_Mapping;
        new_mapping += to_idx;

        old_mapping = (u64 *)CHCORE_PUD_Direct_Mapping;
        old_mapping += from_idx;

        /* We map the available physical memory here. 0~1G has been mapped. */
        while (mem_size > 0 && to_idx < 511 /* 0, 1 to 511 */) {
                /* clear old mapping */
                *old_mapping = 0;
                /* Add new mapping */
                *new_mapping = (from_idx << 30) + PRESENT + WRITABLE + HUGE_1G
                                  + GLOBAL + NX;
                kdebug("set new mapping (%p) to idx %d\n",
                      new_mapping,
                      from_idx);
                mem_size -= SIZE_1G;
                from_idx += 1;
                to_idx += 1;
                new_mapping += 1;
                old_mapping += 1;
        }

        /* Re-setup the direct mapping for all the physical memory */
        new_mapping = (u64 *)CHCORE_PUD_CODE_Mapping;
        old_mapping = (u64 *)CHCORE_PUD_CODE_Mapping;
        
        while (mem_size > 0 && to_idx < 1024) {
                /* Can not map to the last 1GB, mapped to kernel code */
                /* clear old mapping */
                *old_mapping = 0;
                /* Add new mapping */
                *new_mapping = (from_idx << 30) + PRESENT + WRITABLE + HUGE_1G
                                  + GLOBAL + NX;
                mem_size -= SIZE_1G;
                from_idx += 1;
                to_idx += 1;
                new_mapping += 1;
                old_mapping += 1;
        }

        /* Can not mapped all */
        BUG_ON(mem_size > 0);

        /* Flush TLB: SMP is not enabled for now. */
        extern void flush_boot_tlb(void);
        flush_boot_tlb();
}

void parse_mem_map(void *info)
{
        struct multiboot_tag_mmap *tag;
        multiboot_memory_map_t *mmap, *temp;
        paddr_t p_end;
        u64 mlength;
        u64 max_paddr;

        mmap = NULL;
        mlength = 0;
        max_paddr = 0;

        tag = (struct multiboot_tag_mmap *)info;

        /*
         * According to multiboot2 specification,
         * type 1 indicates available memory,
         * type 2 indicates reserved memory,
         * type 3 indicates usable memory holding ACPI information,
         * type 4 indicates reserved memory which needs to be preserved on
         * hibernation, type 5 indicates memory which is occupied by defective
         * RAM modules, type 7 indicates NVM memory, All other types indicate
         * reserved memory.
         */
        for (temp = tag->entries; (u64)temp < (u64)tag + tag->size;
             temp = (multiboot_memory_map_t *)((u64)temp + tag->entry_size)) {
                kinfo("start_addr = 0x%lx, end_addr = 0x%lx, type = 0x%x\n",
                      temp->addr,
                      temp->addr + temp->len,
                      temp->type);

                if (temp->type == MULTIBOOT_MEMORY_AVAILABLE) {
                        if (temp->addr + temp->len > max_paddr)
                                max_paddr = temp->addr + temp->len;

                        if (temp->len > mlength) {
                                mmap = temp;
                                mlength = temp->len;
                        }
                }
        }

        if (mlength == 0) {
                BUG("Failed to detect memory using multiboot2\n");
        }

        /*
         * TODO: use the whole physical memory map according to multiboot2.
         * Currently, ChCore just uses the biggest free chunk.
         */
        BUG_ON(mmap == NULL);
        BUG_ON(mmap->type != MULTIBOOT_MEMORY_AVAILABLE);
        physmem_map_num = 1;

        /* remove kernel image part [0, img_end) */
        p_end = (u64)((void *)img_end - KCODE);
        if (mmap->addr < p_end)
                physmem_map[0][0] = p_end;
        else
                physmem_map[0][0] = mmap->addr;

        physmem_map[0][1] = mmap->addr + mmap->len;

#if 0
        for (int idx = 0; idx < dsm_visible_memdev_num; idx++) {
                p_end = dsm_visible_memdevs[idx].start
                        + dsm_visible_memdevs[idx].size;
                if (p_end > max_paddr)
                        max_paddr = p_end;
        }
        kdebug("max paddr=0x%llx\n", max_paddr);
#endif
        refill_kernel_page_table(max_paddr);

        kinfo("Use dram memory: 0x%lx - 0x%lx\n",
              physmem_map[0][0],
              physmem_map[0][1]);
}

#ifdef DSM_SHM_DEVICE_CXL_NUMA
void parse_numa_mem_map()
{
        u64 cxl_start = 0, cxl_size = 0;
        u64 max_paddr;
        
        dram_numa_mode_setup_mem(&cxl_start, &cxl_size);
        physmem_map[0][0] = cxl_start;
        physmem_map[0][1] = cxl_start + cxl_size;
        max_paddr = physmem_map[0][1];

        physmem_map_num = 1;

        refill_kernel_page_table(max_paddr);

        kinfo("Use NUMA NODE: 0x%lx - 0x%lx\n",
              physmem_map[0][0],
              physmem_map[0][1]);
}
#endif

#ifdef USE_NVM
void parse_nvm_map(void *info)
{
        struct multiboot_tag_mmap *tag;
        multiboot_memory_map_t *temp, *nvmmap;
        u64 nvmlength, max_paddr, nvm_size;
        struct nvm_region nregion;

        nvmmap = NULL;
        nvmlength = 0;
        max_paddr = 0;
        nvm_size = 0;

        if (nvm_region_num == 0) {
                kinfo("[NVM] No nvm region found, try to use info in multiboot_memory_map\n");
                tag = (struct multiboot_tag_mmap *)info;
                for (temp = tag->entries; (u64)temp < (u64)tag + tag->size;
                     temp = (multiboot_memory_map_t *)((u64)temp
                                                       + tag->entry_size)) {
                        if (temp->type == MULTIBOOT_MEMORY_NVM) {
                                if (temp->addr + temp->len > max_paddr)
                                        max_paddr = temp->addr + temp->len;

                                if (temp->len > nvmlength) {
                                        nvmmap = temp;
                                        nvmlength = temp->len;
                                }
                        }
                }
                BUG_ON(nvmmap == NULL);
                BUG_ON(nvmmap->type != MULTIBOOT_MEMORY_NVM);
                nvmmem_map_num = 1;
                nvmmem_map[0][0] = nvmmap->addr;
                nvmmem_map[0][1] = nvmmap->addr + nvmmap->len;
        } else {
                /* NVM infomation is found in NFIT table */
                nvmmem_map_num = nvm_region_num;
                for (int i = 0; i < nvm_region_num; ++i) {
                        nregion = nvm_region_head[i];
                        if (nregion.length > nvmlength) {
                                nvmlength = nregion.length;
                                nvmmem_map[0][0] = nregion.base;
                                nvmmem_map[0][1] =
                                        nregion.base + nregion.length;
                        }

                        if (nregion.base + nregion.length > max_paddr) {
                                max_paddr = nregion.base + nregion.length;
                        }
                }
        }

        if (nvmmem_map[0][0] >= MAX_KERNEL_PG_PADDR)
                BUG("NVM region base exceed paddr limitation\n");

        /* if NVM region exceed max virtual address, resize it */
        if (nvmmem_map[0][1] > MAX_KERNEL_PG_PADDR) {
                nvm_size = (MAX_KERNEL_PG_PADDR - nvmmem_map[0][0])
                           / (128 * SIZE_1G);
                if (nvm_size == 0) {
                        BUG("no enough NVM");
                }
                nvmmem_map[0][1] =
                        nvmmem_map[0][0] + (nvm_size * 128 * SIZE_1G);
                max_paddr = nvmmem_map[0][1];
                kinfo("resize NVM region to %d * 128GB, resize max paddr to 0x%lx\n",
                      nvm_size,
                      max_paddr);
        }

        refill_kernel_page_table(max_paddr);

        kinfo("Use nvm memory: 0x%lx - 0x%lx\n",
              nvmmem_map[0][0],
              nvmmem_map[0][1]);
}
#endif /* USE_NVM */

#ifdef USE_CXL_MEM
void real_cxl_setup_mem(u64 *start, u64 *size)
{
        struct cxl_mem_dev *dev;

        dev = &(cxl_mem_devs[0]);
        *start = cxl_get_memdev_start(dev);
        *size = cxl_get_memdev_end(dev);

        // enable hdm decoder
        // struct cxl_chbs_context *ctx = &cxl_chbs_ctxs[0];
        // cxl_debug("cxl parse chbs: base=%llx\n", ctx->base);

        // struct cxl_component_reg_map map;
        // cxl_probe_component_regs(NULL, (void *)phys_to_virt(ctx->base), &map);
}

void parse_cxlmem_map()
{
        // fill kernel page table for cxl mem devices
        u64 dev_start = 0, dev_size = 0;
        u64 max_paddr = 0;
        cxlmem_map_num = 0;

/* simulate dev use ivshmem or real cxl device */
#if defined(DSM_SHM_DEVICE_IVSHMEM) || defined(DSM_SHM_DEVICE_IVSHMEM_NUMA)
        extern void ivshmem_setup_mem(u64 * start, u64 * end);
        ivshmem_setup_mem(&dev_start, &dev_size);
#elif defined(DSM_SHM_DEVICE_CXL)
        real_cxl_setup_mem(&dev_start, &dev_size);
#elif defined(DSM_SHM_DEVICE_CXL_NUMA)
        /* on spr2, shm device is simulated as NUMA node */
        real_cxl_numa_mode_setup_mem(&dev_start, &dev_size);
#endif
        /* refill page table */
        max_paddr = dev_start + dev_size;
        if (max_paddr >= MAX_KERNEL_PG_PADDR)
                BUG("[CXL] CXL memdev base exceed paddr limitation\n");
        refill_kernel_page_table(max_paddr);

        cxlmem_map[cxlmem_map_num][0] = dev_start + sizeof(dsm_metadata_t);
        cxlmem_map[cxlmem_map_num][1] = dev_start + dev_size;

        /* init dsm metadata */
        dsm_init_meta(phys_to_virt(dev_start));
        dsm_init_mm(dev_start, dev_size, physmem_map[0][0]);
        dsm_add_machine();

        kinfo("[SHM] Use 0x%lx - 0x%lx\n",
              cxlmem_map[cxlmem_map_num][0],
              cxlmem_map[cxlmem_map_num][1]);
        cxlmem_map_num++;

#if 0 /* test the visibility of CXL device write */
        vaddr_t start = phys_to_virt(cxlmem_map[0][0]);
        kinfo("[CXL] start=%llx\n", start);
        if (*(u64 *)start != 0xA) {
                kinfo("[CXL] uninited; init it...\n");
                *(u64 *)start = 0xA;
                FLUSH(start);
                kinfo("[CXL] now start = %lx\n", *(u64 *)start);
        } else {
                kinfo("[CXL] start == 0xA; already inited\n");
        }
        BUG_ON(1);
#endif
}
#endif

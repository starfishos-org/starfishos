#include <common/types.h>
#include <common/kprint.h>
#include <common/macro.h>
#include <common/size.h>
#include <mm/mm.h>
#include <mm/nvm.h>
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
#endif /* USE_NVM */

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
static void parse_cxl_mem_device(struct cxl_mem_dev *devs, int dev_num)
{
        // fill kernel page table for cxl mem devices
        struct cxl_mem_dev *dev;
        u64 max_paddr = 0, dev_start = 0, dev_end = 0;
        int idx = 0;

        if (dev_num == 0)
                return;

        for (idx = 0; idx < dev_num; idx++) {
                dev = &(devs[idx]);
                dev_start = cxl_get_memdev_start(dev);
                dev_end = cxl_get_memdev_end(dev);

                cxlmem_map[cxlmem_map_num][0] = dev_start;
                cxlmem_map[cxlmem_map_num][1] = dev_end;

                // dsm_add_visible_memdev(dev->start, dev->size, 1);

                if (dev_end > max_paddr)
                        max_paddr = dev_end;

                kinfo("Use CXL Fixed Memory Window: 0x%lx - 0x%lx\n",
                      cxlmem_map[cxlmem_map_num][0],
                      cxlmem_map[cxlmem_map_num][1]);

                cxlmem_map_num++;
        }

        if (max_paddr >= MAX_KERNEL_PG_PADDR)
                BUG("[CXL] CXL memdev base exceed paddr limitation\n");

        kinfo("[CXL] max paddr: %llx, paddr limit: %llx\n",
              max_paddr,
              MAX_KERNEL_PG_PADDR);

        refill_kernel_page_table(max_paddr);
}

#if 0
/**
 * FIXME(FN): Map PCI memory to CHCORE_PUD_CODE_Mapping start
 * PCI MEM Base = 512 * 1G (0x40000000) = 0x2800000000
 */
#define PCI_MEM_KBASE (0x2800000000)

void print_binary(u64 value)
{
        u64 mask = 1ULL << 63;

        for (int i = 0; i < 64; ++i) {
                u64 bit = (value & mask) ? 1 : 0;
                printk("%lu", bit);
                mask >>= 1;
        }

        printk("\n");
}

/**
 * ioremap_pci_memory -- map a pci mem region to kernel page table
 * @paddr: start of the pci bar mem resource
 * @mem_size: size of the pci bar mem resource
 * return mapped kernel vaddr
 */
static int ioremap_pci_memory(paddr_t paddr, size_t mem_size, paddr_t *base)
{
        u64 *direct_mapping;
        u64 idx, paddr_idx;

        BUG_ON(!IS_ALIGNED(paddr, SIZE_1G));

        idx = 0;
        paddr_idx = paddr / SIZE_1G;

        kinfo("idx=%d, paddr_idx=%llx\n", idx, paddr_idx);

        /* Re-setup the direct mapping for all the physical memory */
        direct_mapping = (u64 *)CHCORE_PUD_CODE_Mapping;
        *direct_mapping =
                (paddr_idx << 30) + PRESENT + WRITABLE + HUGE_1G + GLOBAL + NX;
        printk("%lx\n", *direct_mapping);
        print_binary(*direct_mapping);

        // while (mem_size >= SIZE_1G) {
        //         /**
        //         *should not map to the last 1GB,
        //         * which is mapped to kernel code
        //         */
        //         if (idx >= 1023) {
        //                 return -1;
        //         }
        //         /* Add mapping for 1G, 2G, 3G ... */
        //         *direct_mapping = (paddr_idx << 30) + PRESENT + WRITABLE
        //                           + HUGE_1G + GLOBAL + NX;
        //         print_binary(*direct_mapping);
        //         mem_size -= SIZE_1G;
        //         direct_mapping += 1;
        //         idx += 1;
        //         paddr_idx += 1;
        // }

        /* Flush TLB: SMP is not enabled for now. */
        extern void flush_boot_tlb(void);
        flush_boot_tlb();

        *base = PCI_MEM_KBASE;
        return 0;
}
#endif

static void parse_kvm_ivshmem_device()
{
        // fill kernel page table for cxl mem devices
        u64 dev_start = 0, dev_size = 0;
        // paddr_t base;
        // int rc;

        extern void ivshmem_setup_mem(u64 * start, u64 * end);
        ivshmem_setup_mem(&dev_start, &dev_size);

#if 0
        rc = ioremap_pci_memory(dev_start, dev_size, &base);
        if (rc) {
                kinfo("mmap pci memory region failed\n");
                return;
        }
#endif
        refill_kernel_page_table(dev_start + dev_size);

        cxlmem_map[cxlmem_map_num][0] = dev_start + sizeof(dsm_metadata_t);
        cxlmem_map[cxlmem_map_num][1] = dev_start + dev_size;

        /* init dsm metadata */
        dsm_init_meta(phys_to_virt(dev_start));
        dsm_add_machine();

        kinfo("Use IVSHMEM (SHM): 0x%lx - 0x%lx\n",
              cxlmem_map[cxlmem_map_num][0],
              cxlmem_map[cxlmem_map_num][1]);
        cxlmem_map_num++;
}

void parse_cxlmem_map()
{
        parse_cxl_mem_device(cxl_mem_devs, cxl_mem_dev_num);

        // enable hdm decoder
        struct cxl_chbs_context *ctx = &cxl_chbs_ctxs[0];
        cxl_debug("cxl parse chbs: base=%llx\n", ctx->base);

        struct cxl_component_reg_map map;
        cxl_probe_component_regs(NULL, (void *)phys_to_virt(ctx->base), &map);

#if 1
        /* Currently, we use ivshmem to simulate CXL Type3 device */
        cxlmem_map_num = 0;
        parse_kvm_ivshmem_device();
#endif

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
